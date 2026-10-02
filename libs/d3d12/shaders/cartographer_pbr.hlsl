// Cartographer forward PBR shader preparation.
//
// The current D3D12 executor exposes b0 as root constants and t0/s0 as the
// first sampled texture/sampler pair. The frame/object/material packing here
// deliberately stays within the public 256-byte root-constant preparation
// bound; a later descriptor tranche can move the same fields into CBVs.

cbuffer PushConstants : register(b0)
{
    float4x4 view_projection;
    float4x4 model;
    float4 camera_position_exposure;
    float4 base_color_factor;
    float4 emissive_factor_and_strength;
    float4 surface_factors;
    float4 light_direction_intensity;
    float4 light_radiance_range;
    float4 shadow_parameters;
};

Texture2D<float4> base_color_texture : register(t0);
SamplerState linear_clamp_sampler : register(s0);

struct VertexInput
{
    float4 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
};

struct VertexOutput
{
    float4 position : SV_POSITION;
    float3 world_position : TEXCOORD0;
    float3 world_normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
};

VertexOutput VSMain(VertexInput input)
{
    VertexOutput output;
    const float4 world_position = mul(model, input.position);
    output.position = mul(view_projection, world_position);
    output.world_position = world_position.xyz;
    output.world_normal = normalize(mul((float3x3)model, input.normal));
    output.uv = input.uv;
    return output;
}

float distribution_ggx(float n_dot_h, float roughness)
{
    const float alpha = max(0.045, roughness * roughness);
    const float alpha_squared = alpha * alpha;
    const float denominator = n_dot_h * n_dot_h * (alpha_squared - 1.0) + 1.0;
    return alpha_squared / (3.14159265 * denominator * denominator);
}

float geometry_schlick_ggx(float n_dot_x, float roughness)
{
    const float k = ((roughness + 1.0) * (roughness + 1.0)) / 8.0;
    return n_dot_x / max(n_dot_x * (1.0 - k) + k, 1e-5);
}

float3 fresnel_schlick(float cos_theta, float3 f0)
{
    return f0 + (1.0 - f0) * pow(1.0 - cos_theta, 5.0);
}

float4 PSMain(VertexOutput input) : SV_TARGET
{
    const float3 normal = normalize(input.world_normal);
    const float3 view = normalize(camera_position_exposure.xyz - input.world_position);
    const float3 light_direction = normalize(light_direction_intensity.xyz);
    const float n_dot_l = max(dot(normal, light_direction), 0.0);
    const float n_dot_v = max(dot(normal, view), 0.0);
    const float3 sampled_base = base_color_texture.Sample(
        linear_clamp_sampler, input.uv).rgb;
    const float3 base_color = sampled_base * base_color_factor.rgb;
    const float metallic = saturate(surface_factors.x);
    const float roughness = saturate(surface_factors.y);
    const float3 f0 = lerp(float3(0.04, 0.04, 0.04), base_color, metallic);
    const float3 half_vector = normalize(view + light_direction);
    const float n_dot_h = max(dot(normal, half_vector), 0.0);
    const float v_dot_h = max(dot(view, half_vector), 0.0);
    const float3 fresnel = fresnel_schlick(v_dot_h, f0);
    const float distribution = distribution_ggx(n_dot_h, roughness);
    const float geometry = geometry_schlick_ggx(n_dot_l, roughness) *
        geometry_schlick_ggx(n_dot_v, roughness);
    const float3 specular = fresnel * distribution * geometry /
        max(4.0 * n_dot_l * n_dot_v, 1e-5);
    const float3 diffuse_weight = (1.0 - fresnel) * (1.0 - metallic);
    const float3 diffuse = diffuse_weight * base_color / 3.14159265;
    const float shadow = saturate(shadow_parameters.w);
    const float3 direct = (diffuse + specular) *
        (n_dot_l * light_radiance_range.rgb * shadow);
    const float3 emissive = emissive_factor_and_strength.rgb *
        emissive_factor_and_strength.w;
    const float3 hdr = max(direct + emissive, 0.0) *
        exp2(camera_position_exposure.w);
    return float4(hdr, base_color_factor.a);
}
