// Cartographer temporal resolve, FP8 (OCP E4M3FN) storage variant.
//
// DXGI has no FP8 texture format, so colour/history/motion are stored as 8-bit
// integer channels (R8G8B8A8_UINT / R8G8_UINT) and decoded/encoded here. All
// arithmetic is fp32; only storage is FP8. This halves the bytes moved per
// pixel relative to the RGBA16F/RG16F kernel at the cost of ALU decode work.

cbuffer TemporalConstants : register(b0)
{
    float4 feedback_and_clamp;
    float4 jitter_and_inverse_extent;
    uint4 history_flags; // x = valid, y = camera cut, w = static-motion assertion
};

Texture2D<uint4> current_color : register(t0);
Texture2D<uint4> history_color : register(t1);
Texture2D<uint2> motion_vectors : register(t2);
RWTexture2D<uint4> resolved_color : register(u0);

static const float kE4M3Max = 448.0f;

// E4M3 (bias 7) shares its bit layout with binary16 (bias 15) shifted by 7 bits,
// so a code placed in the half exponent/mantissa field is exactly the E4M3
// value scaled by 2^-8 (subnormals included). Decode is one f16->f32 convert
// and one multiply; no branches.
float e4m3_decode(uint v)
{
    return f16tof32(((v & 0x80u) << 8u) | ((v & 0x7Fu) << 7u)) * 256.0f;
}

// Round-half-even encode with saturation to +-448, branchless.
uint e4m3_encode(float x)
{
    const uint sign = (asuint(x) >> 24u) & 0x80u;
    const float a = min(abs(x), kE4M3Max);
    const uint bits = asuint(a);
    // Normal range: round the f32 mantissa to 3 bits, rebias the exponent.
    const uint rounded = (bits + 0x7FFFFu + ((bits >> 20u) & 1u)) >> 20u;
    const uint normal_code = rounded - (120u << 3u);
    // Subnormal range (a < 2^-6): the float add rounds a*512 to an integer RNE.
    const uint subnormal_code = asuint(a * 512.0f + 8388608.0f) & 0x7FFFFFu;
    return sign | min(a < (1.0f / 64.0f) ? subnormal_code : normal_code, 0x7Eu);
}

float4 decode4(uint4 v)
{
    return float4(e4m3_decode(v.x), e4m3_decode(v.y), e4m3_decode(v.z), e4m3_decode(v.w));
}

uint4 encode4(float4 v)
{
    return uint4(e4m3_encode(v.x), e4m3_encode(v.y), e4m3_encode(v.z), e4m3_encode(v.w));
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatch_id : SV_DISPATCHTHREADID)
{
    uint width;
    uint height;
    current_color.GetDimensions(width, height);
    if (dispatch_id.x >= width || dispatch_id.y >= height)
    {
        return;
    }

    const int2 pixel = int2(dispatch_id.xy);
    const uint4 current_raw = current_color.Load(int3(pixel, 0));
    if (history_flags.x == 0 || history_flags.y != 0)
    {
        resolved_color[pixel] = current_raw;
        return;
    }

    int2 history_pixel = pixel;
    if (history_flags.w == 0)
    {
        const float2 motion = float2(
            e4m3_decode(motion_vectors.Load(int3(pixel, 0)).x),
            e4m3_decode(motion_vectors.Load(int3(pixel, 0)).y));
        history_pixel = clamp(
            pixel - int2(round(motion)), int2(0, 0), int2(width - 1, height - 1));
    }
    const float4 current = decode4(current_raw);
    const float4 history = decode4(history_color.Load(int3(history_pixel, 0)));
    const float feedback = saturate(feedback_and_clamp.x);
    resolved_color[pixel] = encode4(float4(
        lerp(current.rgb, history.rgb, feedback), current.a));
}
