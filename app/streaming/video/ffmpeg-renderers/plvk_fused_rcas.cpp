#include "plvk_fused_rcas.h"
#include "plvk_rcas.h"

#include <libplacebo/shaders/colorspace.h>
#include <cmath>

void FusedRcas::initialize(pl_log log, pl_gpu gpu)
{
    m_Log = log;
    m_Gpu = gpu;
    m_Hook = {};
    m_Hook.stages = PL_HOOK_OUTPUT;
    m_Hook.input = PL_HOOK_SIG_COLOR;
    m_Hook.priv = this;
    m_Hook.hook = hook;
    m_Hook.signature = UINT64_C(0x4d4c465243415331); // MLFRCAS1
}

void FusedRcas::reset()
{
    // Must run before destruction of the renderer's GPU.
    pl_shader_free(&m_Conversion);
    pl_shader_obj_destroy(&m_ColorMap);
    m_ConversionResult = nullptr;
    m_Luma = m_Chroma = nullptr;
    m_Gpu = nullptr;
    m_Header.clear();
}

#if PL_API_VER >= 372

static bool isPlane(pl_tex tex, int components)
{
    if (!tex || !tex->params.sampleable || tex->params.d ||
            tex->sampler_type != PL_SAMPLER_NORMAL ||
            tex->params.format->type != PL_FMT_UNORM ||
            tex->params.format->num_components != components ||
            !(tex->params.format->caps & PL_FMT_CAP_LINEAR)) {
        return false;
    }
    for (int c = 0; c < components; ++c) {
        if (tex->params.format->sample_order[c] != c)
            return false;
    }
    return true;
}

const pl_hook* FusedRcas::prepare(pl_renderer renderer, const pl_frame& source,
                                const pl_frame& target, float saturation, float strength)
{
    m_Failed = false;
    m_Reason = "unsupported decoded plane layout";
    if (!m_Gpu || source.num_planes != 2 || target.num_planes != 1 ||
            !isPlane(source.planes[0].texture, 1) ||
            !isPlane(source.planes[1].texture, 2) ||
            source.planes[0].components != 1 || source.planes[1].components != 2 ||
            source.planes[0].component_mapping[0] != PL_CHANNEL_Y ||
            source.planes[1].component_mapping[0] != PL_CHANNEL_U ||
            source.planes[1].component_mapping[1] != PL_CHANNEL_V) {
        return nullptr;
    }

    const auto& y = source.planes[0];
    const auto& uv = source.planes[1];
    const int width = y.texture->params.w, height = y.texture->params.h;
    if (width <= 0 || height <= 0 || width % 2 || height % 2 ||
            uv.texture->params.w != width / 2 || uv.texture->params.h != height / 2 ||
            y.texture->params.format->component_depth[0] !=
                    uv.texture->params.format->component_depth[0]) {
        return nullptr;
    }

    m_Reason = "unsupported rotation or plane orientation";
    if (source.rotation || target.rotation || y.flipped || uv.flipped ||
            target.planes[0].flipped || y.shift_x || y.shift_y ||
            target.planes[0].shift_x || target.planes[0].shift_y) {
        return nullptr;
    }

    // Match the renderer's own inference, including missing bit-depth metadata,
    // colorspace defaults, and target representation. The fused path samples
    // the same decoder textures but is no longer restricted to an exact
    // uncropped 1:1 source/target pair.
    pl_frame src = source, dst = target;
    pl_frames_infer(renderer, &src, &dst);

    const float srcW = src.crop.x1 - src.crop.x0;
    const float srcH = src.crop.y1 - src.crop.y0;
    const float outWf = dst.crop.x1 - dst.crop.x0;
    const float outHf = dst.crop.y1 - dst.crop.y0;
    const int outW = int(std::lround(outWf));
    const int outH = int(std::lround(outHf));

    m_Reason = "invalid source crop or output geometry";
    if (srcW <= 0.0f || srcH <= 0.0f || outW <= 0 || outH <= 0 ||
            std::fabs(outWf - outW) > 1e-4f ||
            std::fabs(outHf - outH) > 1e-4f ||
            src.crop.x0 < 0.0f || src.crop.y0 < 0.0f ||
            src.crop.x1 > width || src.crop.y1 > height) {
        return nullptr;
    }

    m_Reason = "requires SDR two-plane YUV and RGB output";
    if (!pl_color_system_is_ycbcr_like(src.repr.sys) ||
            src.repr.alpha != PL_ALPHA_NONE ||
            dst.repr.sys != PL_COLOR_SYSTEM_RGB ||
            pl_color_space_is_hdr(&src.color) ||
            pl_color_space_is_hdr(&dst.color)) {
        return nullptr;
    }

    m_Reason = "additional source processing unsupported by fused RCAS";
    if (src.field != PL_FIELD_NONE || src.film_grain.type != PL_FILM_GRAIN_NONE ||
            src.num_overlays || src.icc || dst.icc || src.profile.len || dst.profile.len ||
            src.lut || dst.lut || src.enhancement_layer) {
        return nullptr;
    }

    // Extra vertex coordinates are needed for sampling the decoder textures.
    if (!pl_find_fmt(m_Gpu, PL_FMT_FLOAT, 2, 32, 32, PL_FMT_CAP_VERTEX)) {
        m_Reason = "no float2 vertex format";
        return nullptr;
    }
    if (!updateConversion(src, dst, saturation)) {
        m_Reason = "unable to construct neighbour color conversion";
        return nullptr;
    }

    m_Luma = y.texture;
    m_Chroma = uv.texture;
    m_LumaAddressMode = y.address_mode;
    m_ChromaAddressMode = uv.address_mode;
    m_TextureSize[0] = float(width);
    m_TextureSize[1] = float(height);
    m_SourceOrigin[0] = src.crop.x0;
    m_SourceOrigin[1] = src.crop.y0;
    m_SourceExtent[0] = srcW;
    m_SourceExtent[1] = srcH;
    m_OutputSize[0] = float(outW);
    m_OutputSize[1] = float(outH);
    m_ChromaShift[0] = uv.shift_x;
    m_ChromaShift[1] = uv.shift_y;
    m_Strength = strength;
    m_Reason = "eligible";
    return &m_Hook;
}

bool FusedRcas::updateConversion(const pl_frame& source, const pl_frame& target,
                                 float saturation)
{
    if (m_ConversionResult && saturation == m_Saturation &&
            pl_color_repr_equal(&source.repr, &m_SourceRepr) &&
            pl_color_repr_equal(&target.repr, &m_TargetRepr) &&
            pl_color_space_equal(&source.color, &m_SourceColor) &&
            pl_color_space_equal(&target.color, &m_TargetColor)) {
        return true;
    }

    pl_shader_params shaderParams = {};
    // This private helper is imported into the single-hook fast pipeline.
    // Reserve the high ID, away from the renderer's handful of plane shaders.
    shaderParams.id = 255;
    shaderParams.gpu = m_Gpu;
    shaderParams.dynamic_constants = true;
    if (m_Conversion)
        pl_shader_reset(m_Conversion, &shaderParams);
    else
        m_Conversion = pl_shader_alloc(m_Log, &shaderParams);
    m_ConversionResult = nullptr;

    pl_color_adjustment adjustment = pl_color_adjustment_neutral;
    adjustment.saturation = saturation;
    pl_color_repr repr = source.repr;
    // Match pass_read_image: normalize sampled storage values before decoding
    // the signal representation. This is essential for P010, where 10-bit
    // signal data is stored in the upper bits of 16-bit UNORM texture samples.
    float sampleScale = pl_color_repr_normalize(&repr);
    pl_shader_var scale = {};
    scale.var = pl_var_float("mlRcasSampleScale");
    scale.data = &sampleScale;
    pl_custom_shader normalize = {};
    normalize.input = normalize.output = PL_SHADER_SIG_COLOR;
    normalize.variables = &scale;
    normalize.num_variables = 1;
    normalize.body = "color.rgb *= mlRcasSampleScale;";
    if (!pl_shader_custom(m_Conversion, &normalize))
        return false;

    pl_color_decode_args decode = {};
    decode.repr = &repr;
    decode.color_adjustment = &adjustment;
    pl_shader_decode_color_ex(m_Conversion, &decode);
    pl_color_map_args map = {};
    map.src = source.color;
    map.dst = target.color;
    map.state = &m_ColorMap;
    pl_shader_color_map_ex(m_Conversion, pl_render_fast_params.color_map_params, &map);
    repr = target.repr;
    pl_color_repr_normalize(&repr);
    repr.alpha = PL_ALPHA_NONE;
    pl_shader_encode_color(m_Conversion, &repr);

    m_ConversionResult = pl_shader_finalize(m_Conversion);
    if (!m_ConversionResult)
        return false;

    m_Header = kRcasCore;
    m_Header += R"GLSL(
vec3 moonlightRcasSample(vec2 outPos)
{
    // RCAS neighbours are defined in final output-pixel space. Map each output
    // pixel center back through the source crop to decoder-plane coordinates.
    // This exactly matches Moonlight's fast direct-sampling path for ordinary
    // scaling while also handling decoder padding/cropping without an RGB FBO.
    outPos = clamp(outPos, vec2(0.5), mlRcasOutputSize - vec2(0.5));
    vec2 srcPos = mlRcasSourceOrigin +
                  outPos * (mlRcasSourceExtent / mlRcasOutputSize);
    float y = texture(mlRcasY, srcPos / mlRcasTextureSize).r;
    vec2 uv = texture(mlRcasUV,
                      (srcPos - mlRcasChromaShift) / mlRcasTextureSize).rg;
    return )GLSL";
    m_Header += m_ConversionResult->name;
    m_Header += "(vec4(y, uv, 1.0)).rgb;\n}\n";
    m_SourceRepr = source.repr;
    m_TargetRepr = target.repr;
    m_SourceColor = source.color;
    m_TargetColor = target.color;
    m_Saturation = saturation;
    return true;
}

pl_hook_res FusedRcas::hook(void* priv, const pl_hook_params* params)
{
    auto& self = *static_cast<FusedRcas*>(priv);
    pl_hook_res result = {};
    // OUTPUT's color is already in the target transfer function, before
    // dithering and overlays. Detect any unexpected renderer transformation.
    if (!self.m_ConversionResult ||
            !pl_color_space_equal(&params->color, &self.m_TargetColor) ||
            !pl_color_repr_equal(params->orig_repr, &self.m_SourceRepr)) {
        self.m_Reason = "renderer output metadata changed";
        self.m_Failed = result.failed = true;
        return result;
    }

    // Import libplacebo's generated color conversion as a callable GLSL
    // function. All associated uniforms/LUT bindings are copied by custom().
    // This only builds shader code: neither helper is dispatched separately.
    const auto& conversion = *self.m_ConversionResult;
    pl_custom_shader import = {};
    import.input = import.output = PL_SHADER_SIG_COLOR;
    import.header = conversion.glsl;
    import.variables = conversion.variables;
    import.num_variables = conversion.num_variables;
    import.constants = conversion.constants;
    import.num_constants = conversion.num_constants;
    import.descriptors = conversion.descriptors;
    import.num_descriptors = conversion.num_descriptors;

    pl_shader_desc descriptors[2] = {};
    descriptors[0].desc.name = "mlRcasY";
    descriptors[1].desc.name = "mlRcasUV";
    descriptors[0].binding.object = self.m_Luma;
    descriptors[1].binding.object = self.m_Chroma;
    descriptors[0].desc.type = PL_DESC_SAMPLED_TEX;
    descriptors[0].binding.sample_mode = PL_TEX_SAMPLE_LINEAR;
    descriptors[0].binding.address_mode = self.m_LumaAddressMode;
    descriptors[1].desc.type = PL_DESC_SAMPLED_TEX;
    descriptors[1].binding.sample_mode = PL_TEX_SAMPLE_LINEAR;
    descriptors[1].binding.address_mode = self.m_ChromaAddressMode;

    pl_shader_var variables[6] = {};
    variables[0].var = pl_var_vec2("mlRcasTextureSize");
    variables[0].data = self.m_TextureSize;
    variables[1].var = pl_var_vec2("mlRcasSourceOrigin");
    variables[1].data = self.m_SourceOrigin;
    variables[2].var = pl_var_vec2("mlRcasSourceExtent");
    variables[2].data = self.m_SourceExtent;
    variables[3].var = pl_var_vec2("mlRcasOutputSize");
    variables[3].data = self.m_OutputSize;
    variables[4].var = pl_var_vec2("mlRcasChromaShift");
    variables[4].data = self.m_ChromaShift;
    variables[5].var = pl_var_float("mlRcasStrength");
    variables[5].data = &self.m_Strength;
    variables[5].dynamic = true;

    const float coordinates[4][2] = {
        {0.0f, 0.0f}, {self.m_OutputSize[0], 0.0f},
        {0.0f, self.m_OutputSize[1]},
        {self.m_OutputSize[0], self.m_OutputSize[1]}
    };
    pl_shader_va position = {};
    position.attr.name = "mlRcasPosition";
    position.attr.fmt = pl_find_fmt(params->gpu, PL_FMT_FLOAT, 2, 32, 32,
                                    PL_FMT_CAP_VERTEX);
    for (int i = 0; i < 4; ++i)
        position.data[i] = coordinates[i];

    pl_custom_shader shader = {};
    shader.input = shader.output = PL_SHADER_SIG_COLOR;
    shader.description = "Moonlight fused RGB RCAS (decoder planes)";
    shader.header = self.m_Header.c_str();
    shader.body = R"GLSL(
        vec3 b = moonlightRcasSample(mlRcasPosition + vec2( 0.0, -1.0));
        vec3 d = moonlightRcasSample(mlRcasPosition + vec2(-1.0,  0.0));
        vec3 f = moonlightRcasSample(mlRcasPosition + vec2( 1.0,  0.0));
        vec3 h = moonlightRcasSample(mlRcasPosition + vec2( 0.0,  1.0));
        color.rgb = moonlightRcas(b, d, color.rgb, f, h, mlRcasStrength);
    )GLSL";
    shader.descriptors = descriptors;
    shader.num_descriptors = 2;
    shader.variables = variables;
    shader.num_variables = 6;
    shader.vertex_attribs = &position;
    shader.num_vertex_attribs = 1;
    // OUTPUT is a non-resizable hook stage. The native hook API requires
    // returned COLOR shaders to carry the exact image dimensions, otherwise
    // pass_hook() rejects the result as an attempted resize.
    shader.output_w = static_cast<int>(self.m_OutputSize[0]);
    shader.output_h = static_cast<int>(self.m_OutputSize[1]);

    if (!pl_shader_custom(params->sh, &import) || !pl_shader_custom(params->sh, &shader)) {
        self.m_Reason = "unable to fuse RCAS into render shader";
        self.m_Failed = result.failed = true;
        return result;
    }
    result.output = PL_HOOK_SIG_COLOR;
    result.sh = params->sh;
    result.rect = params->rect;
    result.repr = params->repr;
    result.color = params->color;
    result.components = params->components;
    return result;
}

#else

// Older system libplacebo builds do not expose the API required by the fused
// hook, so sharpening is unavailable there.
const pl_hook* FusedRcas::prepare(pl_renderer, const pl_frame&, const pl_frame&,
                                float, float)
{
    m_Reason = "requires libplacebo API 372 or newer";
    return nullptr;
}

pl_hook_res FusedRcas::hook(void*, const pl_hook_params*)
{
    return {};
}

#endif
