// Cartographer HDR-to-display tone-map shader preparation.

cbuffer ToneMapConstants : register(b0)
{
    float4 exposure_and_mode; // x = exposure; y = authoring tone-map mode or scene FXAA enable
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
    // Values above this are already white after the bounded display curve.
    // Bound only its evaluation to avoid inf/inf; the HDR image stays intact.
    const float3 x = clamp(value, 0.0, 65536.0);
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

float3 display_color(float2 uv)
{
    const float3 hdr = hdr_color.Sample(linear_clamp_sampler, uv).rgb *
        exp2(exposure_and_mode.x);
    return linear_to_srgb(aces_fitted(hdr));
}

float display_luma(float3 color)
{
    return dot(color, float3(0.299, 0.587, 0.114));
}

float4 PSMain(FullscreenOutput input) : SV_TARGET
{
#ifdef CARTO_AUTHORING_VIEWPORT
    // Exact pixel loads preserve coverage/identity edges in the authored HDR
    // attachment. Its alpha carries bounded draw/primitive identity.
    const float4 source = hdr_color.Load(int3(int2(input.position.xy), 0));
    if (source.a == 0.0) return 0.0;
    // Negative identity is a surface-program numeric fault, never valid HDR.
    // Make it visible without an unconditional full-frame host readback.
    if (source.a < 0.0) return float4(1,0,1,1);
    const float3 hdr = source.rgb * exp2(exposure_and_mode.x);
    const float3 nonnegative = max(hdr, 0.0);
    const float3 mapped = exposure_and_mode.y < 0.5 ? aces_fitted(hdr) :
        (exposure_and_mode.y < 1.5 ? nonnegative / (1.0 + nonnegative) : saturate(hdr));
    return float4(linear_to_srgb(mapped), 1.0);
#else
    const float3 center = display_color(input.uv);
    if (exposure_and_mode.y < 0.5)
        return float4(center, 1.0);

    uint width;
    uint height;
    hdr_color.GetDimensions(width, height);
    const float2 pixel_step = 1.0 / float2(width, height);
    const float3 north_west = display_color(input.uv + pixel_step * float2(-1.0, -1.0));
    const float3 north_east = display_color(input.uv + pixel_step * float2(1.0, -1.0));
    const float3 south_west = display_color(input.uv + pixel_step * float2(-1.0, 1.0));
    const float3 south_east = display_color(input.uv + pixel_step * float2(1.0, 1.0));
    const float luma_center = display_luma(center);
    const float luma_north_west = display_luma(north_west);
    const float luma_north_east = display_luma(north_east);
    const float luma_south_west = display_luma(south_west);
    const float luma_south_east = display_luma(south_east);
    const float luma_min = min(luma_center, min(min(luma_north_west, luma_north_east),
        min(luma_south_west, luma_south_east)));
    const float luma_max = max(luma_center, max(max(luma_north_west, luma_north_east),
        max(luma_south_west, luma_south_east)));
    const float luma_range = luma_max - luma_min;
    if (luma_range < max(0.0312, luma_max * 0.125))
        return float4(center, 1.0);

    float2 direction;
    direction.x = -((luma_north_west + luma_north_east) -
        (luma_south_west + luma_south_east));
    direction.y = (luma_north_west + luma_south_west) -
        (luma_north_east + luma_south_east);
    const float direction_reduce = max(
        (luma_north_west + luma_north_east + luma_south_west + luma_south_east) *
            (0.25 * 0.125), 1.0 / 128.0);
    const float inverse_min_direction = 1.0 /
        (min(abs(direction.x), abs(direction.y)) + direction_reduce);
    direction = clamp(direction * inverse_min_direction,
        float2(-8.0, -8.0), float2(8.0, 8.0)) * pixel_step;
    const float3 filtered_a = 0.5 * (
        display_color(input.uv + direction * (1.0 / 3.0 - 0.5)) +
        display_color(input.uv + direction * (2.0 / 3.0 - 0.5)));
    const float3 filtered_b = filtered_a * 0.5 + 0.25 * (
        display_color(input.uv + direction * -0.5) +
        display_color(input.uv + direction * 0.5));
    const float luma_b = display_luma(filtered_b);
    const float3 result = luma_b < luma_min || luma_b > luma_max
        ? filtered_a : filtered_b;
    return float4(result, 1.0);
#endif
}
