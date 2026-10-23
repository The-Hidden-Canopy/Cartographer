// Cartographer forward PBR shader preparation.
//
// The D3D12 executor exposes b0 as root constants and a bounded t0..t4/s0
// material table. The five texture slots intentionally mirror StandardMaterial
// instead of hiding texture selection in shader-specific conventions.

cbuffer PushConstants : register(b0)
{
    float4x4 view_projection;
    float4x4 model;
    float4 camera_position_exposure;
    float4 base_color_factor;
    float4 emissive_radiance_and_f0;
    float4 surface_factors;
    float4 light_direction_intensity;
    float4 light_radiance_range;
    // The compact float2 keeps this descriptor-heavy root signature at the
    // D3D12 64-DWORD limit.
    float2 clearcoat_factors;
};

Texture2D<float4> base_color_texture : register(t0);
Texture2D<float4> metallic_roughness_texture : register(t1);
Texture2D<float4> normal_texture : register(t2);
Texture2D<float4> occlusion_texture : register(t3);
Texture2D<float4> emissive_texture : register(t4);
SamplerState linear_clamp_sampler : register(s0);

struct VertexInput
{
    float4 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    float4 tangent : TANGENT;
};

struct VertexOutput
{
    float4 position : SV_POSITION;
    float3 world_position : TEXCOORD0;
    float3 world_normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
    float3 world_tangent : TEXCOORD3;
    float tangent_sign : TEXCOORD4;
};

VertexOutput VSMain(VertexInput input)
{
    VertexOutput output;
    const float4 world_position = mul(model, input.position);
    output.position = mul(view_projection, world_position);
    output.world_position = world_position.xyz;
    output.world_normal = normalize(mul((float3x3)model, input.normal));
    output.uv = input.uv;
    output.world_tangent = normalize(mul((float3x3)model, input.tangent.xyz));
    output.tangent_sign = input.tangent.w;
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
    const float3 geometric_normal = normalize(input.world_normal);
    const float3 tangent = normalize(
        input.world_tangent - geometric_normal * dot(geometric_normal, input.world_tangent));
    const float3 bitangent = normalize(cross(geometric_normal, tangent)) * input.tangent_sign;
    float3 tangent_normal = normal_texture.Sample(linear_clamp_sampler, input.uv).xyz * 2.0 - 1.0;
    // The bounded first material contract uses the DirectX Y- convention.
    tangent_normal.y = -tangent_normal.y;
    tangent_normal.xy *= surface_factors.z;
    const float3 normal = normalize(
        tangent * tangent_normal.x + bitangent * tangent_normal.y +
        geometric_normal * tangent_normal.z);
    const float3 view = normalize(camera_position_exposure.xyz - input.world_position);
    const float3 light_direction = normalize(light_direction_intensity.xyz);
    const float n_dot_l = max(dot(normal, light_direction), 0.0);
    const float n_dot_v = max(dot(normal, view), 0.0);
    const float4 sampled_base = base_color_texture.Sample(
        linear_clamp_sampler, input.uv);
    const float3 base_color = sampled_base.rgb * base_color_factor.rgb;
    const float metallic = saturate(surface_factors.x *
        metallic_roughness_texture.Sample(linear_clamp_sampler, input.uv).b);
    const float roughness = saturate(surface_factors.y *
        metallic_roughness_texture.Sample(linear_clamp_sampler, input.uv).g);
    const float dielectric_f0 = saturate(emissive_radiance_and_f0.w);
    const float clearcoat = saturate(clearcoat_factors.x);
    const float clearcoat_roughness = saturate(clearcoat_factors.y);
    const float3 f0 = lerp(
        float3(dielectric_f0, dielectric_f0, dielectric_f0), base_color, metallic);
    const float3 half_vector = normalize(view + light_direction);
    const float n_dot_h = max(dot(normal, half_vector), 0.0);
    const float v_dot_h = max(dot(view, half_vector), 0.0);
    const float3 fresnel = fresnel_schlick(v_dot_h, f0);
    const float clearcoat_fresnel = 0.04 + 0.96 * pow(1.0 - v_dot_h, 5.0);
    const float base_attenuation = saturate(1.0 - clearcoat * clearcoat_fresnel);
    const float distribution = distribution_ggx(n_dot_h, roughness);
    const float geometry = geometry_schlick_ggx(n_dot_l, roughness) *
        geometry_schlick_ggx(n_dot_v, roughness);
    const float3 specular = fresnel * distribution * geometry * base_attenuation /
        max(4.0 * n_dot_l * n_dot_v, 1e-5);
    const float3 diffuse_weight = (1.0 - fresnel) * (1.0 - metallic);
    const float3 diffuse = diffuse_weight * base_color * base_attenuation / 3.14159265;
    const float clearcoat_distribution = distribution_ggx(
        n_dot_h, clearcoat_roughness);
    const float clearcoat_geometry =
        geometry_schlick_ggx(n_dot_l, clearcoat_roughness) *
        geometry_schlick_ggx(n_dot_v, clearcoat_roughness);
    const float3 clearcoat_specular = clearcoat * clearcoat_fresnel *
        clearcoat_distribution * clearcoat_geometry /
        max(4.0 * n_dot_l * n_dot_v, 1e-5);
    const float shadow = saturate(light_radiance_range.w);
    const float occlusion = lerp(
        1.0,
        saturate(occlusion_texture.Sample(linear_clamp_sampler, input.uv).r),
        saturate(surface_factors.w));
    const float3 direct = (diffuse + specular + clearcoat_specular) *
        (n_dot_l * light_radiance_range.rgb * shadow);
    const float3 ambient = base_color * (0.03 * occlusion);
    const float3 emissive = emissive_texture.Sample(linear_clamp_sampler, input.uv).rgb *
        emissive_radiance_and_f0.rgb;
    const float alpha = sampled_base.a * base_color_factor.a;
    // A negative light-direction w encodes masked alpha and its cutoff. The
    // positive path remains opaque/blended-compatible for the current pass.
    if (light_direction_intensity.w < 0.0 &&
        alpha < -light_direction_intensity.w) discard;
    const float3 hdr = max(direct + ambient + emissive, 0.0) *
        exp2(camera_position_exposure.w);
    return float4(hdr, alpha);
}
