// Cartographer depth-only shadow-map shader preparation.

cbuffer ShadowConstants : register(b0)
{
    float4x4 light_view_projection : packoffset(c0);
    float4x4 model : packoffset(c4);
    float alpha_cutoff : packoffset(c8.x);
    float base_color_alpha : packoffset(c8.y);
    float alpha_test_enabled : packoffset(c8.z);
    float base_color_alpha_channel : packoffset(c8.w);
};

// The host supplies the material's transformed base-color UV stream.
struct VertexInput
{
    float4 position : POSITION;
    float2 uv : TEXCOORD0;
    float4 vertex_color : COLOR0;
};

struct VertexOutput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
    float4 vertex_color : COLOR0;
};

Texture2D<float4> base_color_texture : register(t0);
SamplerState linear_clamp_sampler : register(s0);

float mapped_alpha(float4 sample_value, float selector)
{
    if (!isfinite(selector))
        return 0.0;
    const uint channel = (uint)selector;
    if (channel < 4U) return sample_value[channel];
    if (channel < 8U) return 1.0 - sample_value[channel - 4U];
    if (channel == 9U) return 1.0;
    return 0.0;
}

VertexOutput VSMain(VertexInput input)
{
    VertexOutput output;
    output.position = mul(light_view_projection, mul(model, input.position));
    output.uv = input.uv;
    output.vertex_color = input.vertex_color;
    return output;
}

struct PixelOutput
{
    float4 color_depth : SV_TARGET;
    float hardware_depth : SV_DEPTH;
};

PixelOutput PSMain(VertexOutput input)
{
    if (alpha_test_enabled > 0.5)
    {
        const float alpha = mapped_alpha(
            base_color_texture.Sample(linear_clamp_sampler, input.uv),
            base_color_alpha_channel) * base_color_alpha * input.vertex_color.a;
        if (alpha < alpha_cutoff)
            discard;
    }
    // Pixel-stage SV_POSITION.z is already the rasterized [0,1] depth. Its w
    // is reciprocal clip-space w, so dividing again corrupts the shadow map.
    const float depth = input.position.z;
    PixelOutput output;
    output.color_depth = float4(depth, depth, depth, 1.0);
    output.hardware_depth = depth;
    return output;
}
