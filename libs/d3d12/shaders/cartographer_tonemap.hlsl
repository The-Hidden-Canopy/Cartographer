// Cartographer HDR-to-display tone-map shader preparation.

cbuffer ToneMapConstants : register(b0)
{
    float4 exposure_and_mode; // x = exposure compensation, y = mode
};

Texture2D<float4> hdr_color : register(t0);
SamplerState linear_clamp_sampler : register(s0);

struct FullscreenOutput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

FullscreenOutput VSMain(uint vertex_id : SV_VERTEXID)
{
    FullscreenOutput output;
    const float2 positions[3] = {
        float2(-1.0, -1.0), float2(-1.0, 3.0), float2(3.0, -1.0)};
    const float2 uvs[3] = {
        float2(0.0, 1.0), float2(0.0, -1.0), float2(2.0, 1.0)};
    output.position = float4(positions[vertex_id], 0.0, 1.0);
    output.uv = uvs[vertex_id];
    return output;
}

float3 aces_fitted(float3 value)
{
    const float3 x = max(value, 0.0);
    const float3 numerator = x * (2.51 * x + 0.03);
    const float3 denominator = x * (2.43 * x + 0.59) + 0.14;
    return saturate(numerator / denominator);
}

float3 linear_to_srgb(float3 value)
{
    const float3 clamped = saturate(value);
    const float3 encoded = 1.055 * pow(clamped, 1.0 / 2.4) - 0.055;
    return lerp(encoded, clamped * 12.92,
                step(clamped, float3(0.0031308, 0.0031308, 0.0031308)));
}

float4 PSMain(FullscreenOutput input) : SV_TARGET
{
    const float3 hdr = hdr_color.Sample(linear_clamp_sampler, input.uv).rgb *
        exp2(exposure_and_mode.x);
    return float4(linear_to_srgb(aces_fitted(hdr)), 1.0);
}
