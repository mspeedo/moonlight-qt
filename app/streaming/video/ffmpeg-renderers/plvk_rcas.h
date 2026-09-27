#pragma once

// FP32 RCAS core used by the fused decoder-plane sharpening hook.
static const char kRcasCore[] = R"RCAS(
float rcasSafeRcp(float x)
{
    return abs(x) > 1e-6 ? 1.0 / x : 0.0;
}

vec3 moonlightRcas(vec3 b, vec3 d, vec3 e, vec3 f, vec3 h, float sharpening)
{
    // Current FidelityFX RCAS luma/noise detector. RCAS operates on the
    // five-tap cross and reduces sharpening on likely noise/grain.
    float bL = b.b * 0.5 + b.r * 0.5 + b.g;
    float dL = d.b * 0.5 + d.r * 0.5 + d.g;
    float eL = e.b * 0.5 + e.r * 0.5 + e.g;
    float fL = f.b * 0.5 + f.r * 0.5 + f.g;
    float hL = h.b * 0.5 + h.r * 0.5 + h.g;
    float maxL = max(max(max(bL, dL), max(eL, fL)), hL);
    float minL = min(min(min(bL, dL), min(eL, fL)), hL);
    float nz = 0.25 * (bL + dL + fL + hL) - eL;
    nz = clamp(abs(nz) * rcasSafeRcp(maxL - minL), 0.0, 1.0);
    nz = -0.5 * nz + 1.0;

    // Ring min/max and the modern RCAS clipping limiters. The lower
    // limiter multiplier is the current FidelityFX fix that prevents
    // possible negative RCAS output in difficult dark-edge cases.
    vec3 mn4 = min(min(b, d), min(f, h));
    vec3 mx4 = max(max(b, d), max(f, h));
    float minRingL = min(min(bL, dL), min(fL, hL));
    float lowerLimiterMultiplier = clamp(eL / max(minRingL, 1e-6), 0.0, 1.0);

    vec3 hitMin = mn4 * vec3(
        rcasSafeRcp(4.0 * mx4.r),
        rcasSafeRcp(4.0 * mx4.g),
        rcasSafeRcp(4.0 * mx4.b)) * lowerLimiterMultiplier;
    vec3 hitMax = (vec3(1.0) - mx4) * vec3(
        rcasSafeRcp(4.0 * mn4.r - 4.0),
        rcasSafeRcp(4.0 * mn4.g - 4.0),
        rcasSafeRcp(4.0 * mn4.b - 4.0));

    vec3 lobeRGB = max(-hitMin, hitMax);
    float lobe = max(-0.1875, min(max(max(lobeRGB.r, lobeRGB.g), lobeRGB.b), 0.0));

    // FsrRcasCon() converts its public 'stops' control to a 0..1 linear
    // multiplier with exp2(-stops). Moonlight exposes that resulting
    // multiplier directly: 0.0 = true bypass, 1.0 = maximum RCAS.
    lobe *= clamp(sharpening, 0.0, 1.0);

    // Match the current FSR3 RCAS path with denoise enabled.
    lobe *= nz;

    float rcpL = rcasSafeRcp(4.0 * lobe + 1.0);
    vec3 sharpened = (lobe * (b + d + h + f) + e) * rcpL;
    return sharpened;
}
)RCAS";
