// fsrlite upscale passes.
// Two stages, same shape as the FSR1 pipeline:
//   PSEASU - spatial upscale of the captured frame
//   PSRCAS - contrast-adaptive sharpen on the upscaled image
// PSCopy  - plain blit, used by the bypass hotkey
// Compiled at runtime from this file, no offline fxc step.

cbuffer Params : register(b0)
{
    float2 SrcSize;      // size of the texture bound to t0, in pixels
    float2 DstSize;      // size of the render target (before letterboxing)
    float  Sharpness;    // RCAS strength, 0..2
    float  EdgeStrength; // bilateral falloff for EASU, ~4..24
    float2 _pad;
};

Texture2D<float4> Src : register(t0);
SamplerState LinSmp  : register(s0);

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VSOut VSFullscreen(uint id : SV_VertexID)
{
    VSOut o;
    // fullscreen triangle: (0,0) (2,0) (0,2)
    float2 p = float2((id << 1) & 2, id & 2);
    o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0);
    o.uv  = float2(p.x, 1.0 - p.y);
    return o;
}

float Luma(float3 c)
{
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

// Lanczos2 kernel, support [-2, 2]
float Lanczos2(float t)
{
    t = abs(t);
    if (t < 1e-5) return 1.0;
    if (t >= 2.0) return 0.0;
    float pt = 3.14159265358979 * t;
    return (2.0 * sin(pt) * sin(0.5 * pt)) / (pt * pt);
}

// ---------------------------------------------------------------- EASU
// 12-tap edge-aware upscale over the 4x4 neighbourhood minus corners:
//   . b c .
//   e f g h
//   i j k l
//   . n o .
// Taps are weighted by a separable Lanczos2 in source-pixel space and
// attenuated by local luma difference, so sharp edges don't bleed into the
// flat regions next to them.
float4 PSEASU(VSOut i) : SV_Target
{
    float2 texel = rcp(SrcSize);
    float2 ip    = i.uv * SrcSize - 0.5;
    float2 fp    = floor(ip);
    float2 f     = ip - fp;

    float3 c0 = Src.SampleLevel(LinSmp, i.uv, 0).rgb;
    float  l0 = Luma(c0);

    float3 acc  = c0;
    float  wsum = 1.0;

    [unroll]
    for (int y = -1; y <= 2; ++y)
    {
        [unroll]
        for (int x = -1; x <= 2; ++x)
        {
            if (x == 0 && y == 0) continue;

            float w = Lanczos2(f.x - (float)x) * Lanczos2(f.y - (float)y);
            if (w <= 0.0) continue;

            float2 o = float2((float)x, (float)y);
            float3 c = Src.SampleLevel(LinSmp, (fp + o + 0.5) * texel, 0).rgb;
            w *= exp(-abs(Luma(c) - l0) * EdgeStrength);

            if (w > 0.0)
            {
                acc  += c * w;
                wsum += w;
            }
        }
    }

    return float4(acc / max(wsum, 1e-5), 1.0);
}

// ---------------------------------------------------------------- RCAS
// Cross-shaped unsharp with local-contrast damping: flat, grainy areas get
// almost no sharpening, real edges get the full amount. That is what keeps
// the result from turning into crunchy aliasing.
float4 PSRCAS(VSOut i) : SV_Target
{
    float2 uv = i.uv;
    float2 t  = rcp(DstSize);

    float3 e = Src.SampleLevel(LinSmp, uv, 0).rgb;
    float3 b = Src.SampleLevel(LinSmp, uv + float2(0.0, -t.y), 0).rgb;
    float3 d = Src.SampleLevel(LinSmp, uv + float2(-t.x, 0.0), 0).rgb;
    float3 f = Src.SampleLevel(LinSmp, uv + float2(t.x, 0.0), 0).rgb;
    float3 h = Src.SampleLevel(LinSmp, uv + float2(0.0, t.y), 0).rgb;

    float3 mn = min(min(min(b, d), min(f, h)), e);
    float3 mx = max(max(max(b, d), max(f, h)), e);

    float contrast = max(mx.r - mn.r, max(mx.g - mn.g, mx.b - mn.b));
    float flatness = saturate(1.0 - contrast * 4.0);

    float  s    = Sharpness * (1.0 - flatness * 0.6);
    float3 blur = (b + d + f + h) * 0.25;

    return float4(saturate(e + (e - blur) * s), 1.0);
}

// ---------------------------------------------------------------- copy
// Plain resampled blit, used by the bypass hotkey.
float4 PSCopy(VSOut i) : SV_Target
{
    return float4(Src.SampleLevel(LinSmp, i.uv, 0).rgb, 1.0);
}
