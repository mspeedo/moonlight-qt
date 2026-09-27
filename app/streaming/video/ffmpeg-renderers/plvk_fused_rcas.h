#pragma once

#include <libplacebo/renderer.h>
#include <libplacebo/shaders/custom.h>
#include <string>

// A native COLOR -> COLOR hook. It appends sampling and RCAS to the final
// render shader; it never allocates or dispatches a full-frame texture.
class FusedRcas {
public:
    ~FusedRcas() { reset(); }
    FusedRcas() = default;
    FusedRcas(const FusedRcas&) = delete;
    FusedRcas& operator=(const FusedRcas&) = delete;

    void initialize(pl_log log, pl_gpu gpu);
    void reset();
    const pl_hook* prepare(pl_renderer renderer, const pl_frame& source,
                           const pl_frame& target, float saturation, float strength);
    const char* reason() const { return m_Reason; }
    bool failed() const { return m_Failed; }

private:
    static pl_hook_res hook(void* priv, const pl_hook_params* params);
    bool updateConversion(const pl_frame& source, const pl_frame& target,
                          float saturation);

    pl_log m_Log = nullptr;
    pl_gpu m_Gpu = nullptr;
    pl_hook m_Hook = {};
    pl_shader m_Conversion = nullptr;
    pl_shader_obj m_ColorMap = nullptr;
    const pl_shader_res* m_ConversionResult = nullptr;
    pl_color_repr m_SourceRepr = {}, m_TargetRepr = {};
    pl_color_space m_SourceColor = {}, m_TargetColor = {};
    float m_Saturation = 1.0f, m_Strength = 0.0f;
    pl_tex m_Luma = nullptr, m_Chroma = nullptr;
    pl_tex_address_mode m_LumaAddressMode = PL_TEX_ADDRESS_CLAMP;
    pl_tex_address_mode m_ChromaAddressMode = PL_TEX_ADDRESS_CLAMP;
    float m_Size[2] = {}, m_ChromaShift[2] = {};
    std::string m_Header;
    const char* m_Reason = "not prepared";
    bool m_Failed = false;
};
