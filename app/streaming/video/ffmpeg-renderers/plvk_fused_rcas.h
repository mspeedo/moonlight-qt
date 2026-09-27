#pragma once

#include <libplacebo/renderer.h>
#include <libplacebo/shaders/custom.h>

// Tiny COLOR -> COLOR RCAS hook. All source sampling, chroma alignment,
// normalization, scaling and color conversion are owned by the patched
// libplacebo OUTPUT color-sampler extension.
class FusedRcas {
public:
    void initialize();
    void reset() {}

    const pl_hook* prepare(float strength);
    const char* reason() const { return m_Reason; }
    bool failed() const { return m_Failed; }
    bool availabilityKnown() const { return m_AvailabilityKnown; }
    bool available() const { return m_Available; }

private:
    static pl_hook_res hook(void* priv, const pl_hook_params* params);

    pl_hook m_Hook = {};
    float m_Strength = 0.0f;
    const char* m_Reason = "not probed";
    bool m_Failed = false;
    bool m_AvailabilityKnown = false;
    bool m_Available = false;
};
