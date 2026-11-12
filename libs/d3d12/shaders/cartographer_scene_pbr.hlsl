// Cartographer forward PBR shader preparation.
//
// The D3D12 executor exposes b0 as root constants and bounded t0..t8/s0..s4
// material tables. The five material slots mirror StandardMaterial; t5 is the
// single key-directional or spotlight shadow map, t6 stores diffuse SH and
// panorama mips, and t7 stores the split-sum GGX BRDF integration lookup.

cbuffer PushConstants : register(b0)
{
    float4x4 view_projection : packoffset(c0);
    float4x4 model : packoffset(c4);
    float4 base_color_factor : packoffset(c8);
    float4 emissive_radiance_and_f0 : packoffset(c9);
    float4 light_direction_intensity : packoffset(c10);
    float3 light_radiance : packoffset(c11.x);
    float clearcoat_roughness : packoffset(c11.w);
};

Texture2D<float4> base_color_texture : register(t0);
Texture2D<float4> metallic_roughness_texture : register(t1);
Texture2D<float4> normal_texture : register(t2);
Texture2D<float4> occlusion_texture : register(t3);
Texture2D<float4> emissive_texture : register(t4);
Texture2D<float4> shadow_depth_texture : register(t5);
Texture2D<float4> scene_environment : register(t6);
Texture2D<float4> split_sum_brdf_lut : register(t7);
// Four RGBA32F texels per row: position/range, emission direction/kind,
// linear radiance/inner-cone cosine, then outer-cone cosine/shadow flag.
Texture2D<float4> authored_local_lights : register(t8);
SamplerState base_color_sampler : register(s0);
SamplerState metallic_roughness_sampler : register(s1);
SamplerState normal_sampler : register(s2);
SamplerState occlusion_sampler : register(s3);
SamplerState emissive_sampler : register(s4);

float3 normalize_or(float3 value, float3 fallback)
{
    const float value_length_squared = dot(value, value);
    if (!all(isfinite(value)) || !isfinite(value_length_squared) ||
        value_length_squared <= 1e-20)
    {
        const float fallback_length_squared = dot(fallback, fallback);
        if (!all(isfinite(fallback)) || !isfinite(fallback_length_squared) ||
            fallback_length_squared <= 1e-20)
            return float3(0.0, 0.0, 1.0);
        return fallback * rsqrt(fallback_length_squared);
    }
    return value * rsqrt(value_length_squared);
}

// The host bakes each authored texture transform into its own UV stream. This
// keeps the fixed root-constant block within the D3D12 signature budget.
struct VertexInput
{
    float4 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    float4 tangent : TANGENT;
    float4 light_position : TEXCOORD1;
    float2 metallic_roughness_uv : TEXCOORD2;
    float2 normal_uv : TEXCOORD3;
    float2 occlusion_uv : TEXCOORD4;
    float2 emissive_uv : TEXCOORD5;
    float4 camera_clearcoat : TEXCOORD6;
    float4 material_surface_factors : TEXCOORD7;
    float4 material_flags : TEXCOORD8;
    float4 vertex_color : COLOR0;
    float4 channel_mapping_a : TEXCOORD9;
    float4 channel_mapping_b : TEXCOORD10;
    float4 channel_mapping_c : TEXCOORD11;
};

struct VertexOutput
{
    float4 position : SV_POSITION;
    float3 world_position : TEXCOORD0;
    float3 world_normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
    float3 world_tangent : TEXCOORD3;
    float tangent_sign : TEXCOORD4;
    float4 light_position : TEXCOORD5;
    float2 metallic_roughness_uv : TEXCOORD6;
    float2 normal_uv : TEXCOORD7;
    float2 occlusion_uv : TEXCOORD8;
    float2 emissive_uv : TEXCOORD9;
    nointerpolation float4 camera_clearcoat : TEXCOORD10;
    nointerpolation float4 material_surface_factors : TEXCOORD11;
    nointerpolation float4 material_flags : TEXCOORD12;
    float4 vertex_color : COLOR0;
    nointerpolation float4 channel_mapping_a : TEXCOORD13;
    nointerpolation float4 channel_mapping_b : TEXCOORD14;
    nointerpolation float4 channel_mapping_c : TEXCOORD15;
};

VertexOutput VSMain(VertexInput input)
{
    VertexOutput output;
    const float4 world_position = mul(model, input.position);
    output.position = mul(view_projection, world_position);
    output.world_position = world_position.xyz;
    // Inverse-transpose via cofactors preserves the 64-DWORD root signature.
    // The host rejects singular transforms. HLSL indexes rows, so select the
    // model columns explicitly; mirrored models also reverse tangent parity.
    const float3 c0 = float3(model[0][0], model[1][0], model[2][0]);
    const float3 c1 = float3(model[0][1], model[1][1], model[2][1]);
    const float3 c2 = float3(model[0][2], model[1][2], model[2][2]);
    const float orientation = dot(c0, cross(c1, c2)) < 0.0 ? -1.0 : 1.0;
    const float3 world_normal = orientation * (
        cross(c1, c2) * input.normal.x + cross(c2, c0) * input.normal.y +
        cross(c0, c1) * input.normal.z);
    output.world_normal = normalize_or(world_normal, float3(0.0, 0.0, 1.0));
    const float3 fallback_axis = abs(output.world_normal.z) < 0.999
        ? float3(0.0, 0.0, 1.0) : float3(0.0, 1.0, 0.0);
    const float3 fallback_tangent = normalize_or(
        cross(fallback_axis, output.world_normal), float3(1.0, 0.0, 0.0));
    output.uv = input.uv;
    output.world_tangent = normalize_or(
        mul((float3x3)model, input.tangent.xyz), fallback_tangent);
    output.tangent_sign = input.tangent.w * orientation;
    output.light_position = input.light_position;
    output.metallic_roughness_uv = input.metallic_roughness_uv;
    output.normal_uv = input.normal_uv;
    output.occlusion_uv = input.occlusion_uv;
    output.emissive_uv = input.emissive_uv;
    output.camera_clearcoat = input.camera_clearcoat;
    output.material_surface_factors = input.material_surface_factors;
    output.material_flags = input.material_flags;
    output.vertex_color = input.vertex_color;
    output.channel_mapping_a = input.channel_mapping_a;
    output.channel_mapping_b = input.channel_mapping_b;
    output.channel_mapping_c = input.channel_mapping_c;
    return output;
}

float mapped_channel(float4 sample_value, float selector)
{
    if (!isfinite(selector))
        return 0.0;
    const uint channel = (uint)selector;
    if (channel < 4U) return sample_value[channel];
    if (channel < 8U) return 1.0 - sample_value[channel - 4U];
    if (channel == 9U) return 1.0;
    return 0.0;
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

float shadow_visibility(float4 light_position, float3 normal, float3 light_direction)
{
    if (!all(isfinite(light_position)) || light_position.w <= 1e-5)
        return 1.0;
    const float3 projected = light_position.xyz / light_position.w;
    if (!all(isfinite(projected)))
        return 1.0;
    if (projected.x < -1.0 || projected.x > 1.0 ||
        projected.y < -1.0 || projected.y > 1.0 ||
        projected.z < 0.0 || projected.z > 1.0)
        return 1.0;

    uint width = 0U;
    uint height = 0U;
    shadow_depth_texture.GetDimensions(width, height);
    if (width == 0U || height == 0U)
        return 1.0;
    const float2 uv = float2(projected.x * 0.5 + 0.5, 0.5 - projected.y * 0.5);
    const int2 dimensions = int2(width, height);
    const int2 center = int2(uv * float2(width, height));
    const float bias = max(0.0005, 0.0015 * (1.0 - saturate(dot(normal, light_direction))));
    float visibility = 0.0;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            const int2 pixel = center + int2(x, y);
            if (pixel.x < 0 || pixel.y < 0 || pixel.x >= dimensions.x || pixel.y >= dimensions.y)
            {
                visibility += 1.0;
                continue;
            }
            const float closest = shadow_depth_texture.Load(int3(pixel, 0)).r;
            visibility += projected.z - bias <= closest ? 1.0 : 0.0;
        }
    }
    return visibility / 9.0;
}

float3 evaluate_diffuse_irradiance_over_pi(float3 normal)
{
    // Real order-2 SH basis: l0, l1(y,z,x), then l2(xy,yz,3z^2-1,xz,x^2-y^2).
    // Input coefficients already include Lambert convolution and division by pi.
    const float x = normal.x;
    const float y = normal.y;
    const float z = normal.z;
    const float3 c0 = scene_environment.Load(int3(0, 0, 0)).rgb;
    const float3 c1 = scene_environment.Load(int3(1, 0, 0)).rgb;
    const float3 c2 = scene_environment.Load(int3(2, 0, 0)).rgb;
    const float3 c3 = scene_environment.Load(int3(0, 1, 0)).rgb;
    const float3 c4 = scene_environment.Load(int3(1, 1, 0)).rgb;
    const float3 c5 = scene_environment.Load(int3(2, 1, 0)).rgb;
    const float3 c6 = scene_environment.Load(int3(0, 2, 0)).rgb;
    const float3 c7 = scene_environment.Load(int3(1, 2, 0)).rgb;
    const float3 c8 = scene_environment.Load(int3(2, 2, 0)).rgb;
    const float3 value = c0 * 0.282095 +
        c1 * (0.488603 * y) + c2 * (0.488603 * z) + c3 * (0.488603 * x) +
        c4 * (1.092548 * x * y) + c5 * (1.092548 * y * z) +
        c6 * (0.315392 * (3.0 * z * z - 1.0)) +
        c7 * (1.092548 * x * z) + c8 * (0.546274 * (x * x - y * y));
    return max(value, 0.0);
}

float3 sample_environment_bilinear(float2 uv, uint mip_level)
{
    uint width = 0U;
    uint height = 0U;
    uint mip_count = 0U;
    scene_environment.GetDimensions(mip_level, width, height, mip_count);
    if (mip_count == 0U || mip_level >= mip_count || width == 0U || height == 0U)
        return 0.0;

    const float2 texel = float2(frac(uv.x), saturate(uv.y)) *
        float2(width, height) - 0.5;
    const int2 base = int2(floor(texel));
    const int signed_width = int(width);
    const int wrapped_x0 = ((base.x % signed_width) + signed_width) % signed_width;
    const int wrapped_x1 = (wrapped_x0 + 1) % signed_width;
    const int y0 = clamp(base.y, 0, int(height) - 1);
    const int y1 = clamp(base.y + 1, 0, int(height) - 1);
    const float blend_x = frac(texel.x);
    const float blend_y = saturate(texel.y - float(y0));
    const float3 top = lerp(
        scene_environment.Load(int3(wrapped_x0, y0, int(mip_level))).rgb,
        scene_environment.Load(int3(wrapped_x1, y0, int(mip_level))).rgb,
        blend_x);
    const float3 bottom = lerp(
        scene_environment.Load(int3(wrapped_x0, y1, int(mip_level))).rgb,
        scene_environment.Load(int3(wrapped_x1, y1, int(mip_level))).rgb,
        blend_x);
    return lerp(top, bottom, blend_y);
}

float3 sample_specular_environment_level(
    float2 uv, uint mip_level, uint mip_levels)
{
    if (mip_level == 0U && mip_levels > 1U)
    {
        uint width = 0U;
        uint height = 0U;
        uint actual_mip_levels = 0U;
        scene_environment.GetDimensions(0U, width, height, actual_mip_levels);
        const float2 texel = float2(frac(uv.x), saturate(uv.y)) *
            float2(width, height) - 0.5;
        const int2 base = int2(floor(texel));
        const int signed_width = int(width);
        const int x0 = ((base.x % signed_width) + signed_width) % signed_width;
        const int x1 = (x0 + 1) % signed_width;
        const int y0 = clamp(base.y, 0, int(height) - 1);
        const int y1 = clamp(base.y + 1, 0, int(height) - 1);
        const bool overlaps_sh = (x0 < 3 || x1 < 3) && (y0 < 3 || y1 < 3);
        if (overlaps_sh && actual_mip_levels > 1U)
            return sample_environment_bilinear(uv, 1U);
    }
    return sample_environment_bilinear(uv, mip_level);
}

float3 sample_specular_environment(
    float3 direction, float roughness, uint mip_levels)
{
    const float3 unit_direction = normalize_or(direction, float3(0.0, 0.0, 1.0));
    const float u = frac(0.5 + atan2(unit_direction.x, unit_direction.z) /
        (2.0 * 3.14159265));
    const float v = acos(clamp(unit_direction.y, -1.0, 1.0)) / 3.14159265;
    // Mip zero stores SH in its upper-left 3x3. GGX-prefiltered levels are
    // selected across the full perceptual roughness range; samples touching
    // the packed SH patch fall back to mip one. Longitude wraps and latitude
    // clamps at the poles.
    const float maximum_lod = max(1.0, float(mip_levels - 1U));
    const float lod = saturate(roughness) * maximum_lod;
    const uint lower_mip = uint(floor(lod));
    const uint upper_mip = min(lower_mip + 1U, mip_levels - 1U);
    const float3 lower = sample_specular_environment_level(
        float2(u, v), lower_mip, mip_levels);
    const float3 upper = sample_specular_environment_level(
        float2(u, v), upper_mip, mip_levels);
    return lerp(lower, upper, frac(lod));
}

float2 sample_split_sum_brdf(float n_dot_v, float roughness)
{
    uint width = 0U;
    uint height = 0U;
    split_sum_brdf_lut.GetDimensions(width, height);
    if (width == 0U || height == 0U)
        return float2(0.0, 0.0);

    const float2 texel = saturate(float2(n_dot_v, roughness)) *
        float2(width, height) - 0.5;
    const int2 base = int2(floor(texel));
    const int2 maximum = int2(width, height) - 1;
    const int2 p00 = clamp(base, int2(0, 0), maximum);
    const int2 p10 = clamp(base + int2(1, 0), int2(0, 0), maximum);
    const int2 p01 = clamp(base + int2(0, 1), int2(0, 0), maximum);
    const int2 p11 = clamp(base + int2(1, 1), int2(0, 0), maximum);
    const float2 top = lerp(
        split_sum_brdf_lut.Load(int3(p00, 0)).rg,
        split_sum_brdf_lut.Load(int3(p10, 0)).rg,
        frac(texel.x));
    const float2 bottom = lerp(
        split_sum_brdf_lut.Load(int3(p01, 0)).rg,
        split_sum_brdf_lut.Load(int3(p11, 0)).rg,
        frac(texel.x));
    return lerp(top, bottom, frac(texel.y));
}

// Matches evaluate_scene_sheen_reference. Scalar sheen is a mixture with the
// base, below clearcoat, with base-color tint and shared roughness. Metallic
// suppresses the mixture weight; full metals retain their existing base lobe.
// The host refuses sheen with environment lighting until a sheen IBL exists.
float sheen_charlie_neubelt(float roughness, float n_dot_l, float n_dot_v,
    float n_dot_h)
{
    if (n_dot_l <= 0.0 || n_dot_v <= 0.0)
        return 0.0;
    const float inverse_alpha = 1.0 / max(0.07, roughness * roughness);
    const float distribution = (2.0 + inverse_alpha) *
        pow(saturate(1.0 - n_dot_h * n_dot_h), 0.5 * inverse_alpha) /
        (2.0 * 3.14159265);
    const float visibility = 1.0 / max(1.0e-5,
        4.0 * (n_dot_l + n_dot_v - n_dot_l * n_dot_v));
    return distribution * visibility;
}

float4 PSMain(VertexOutput input) : SV_TARGET
{
    const float3 geometric_normal = normalize_or(input.world_normal, float3(0.0, 0.0, 1.0));
    const float3 tangent_axis = abs(geometric_normal.z) < 0.999
        ? float3(0.0, 0.0, 1.0) : float3(0.0, 1.0, 0.0);
    const float3 fallback_tangent = normalize_or(
        cross(tangent_axis, geometric_normal), float3(1.0, 0.0, 0.0));
    const float3 tangent = normalize_or(
        input.world_tangent - geometric_normal * dot(geometric_normal, input.world_tangent),
        fallback_tangent);
    const float tangent_sign = input.tangent_sign < 0.0 ? -1.0 : 1.0;
    const float3 bitangent = normalize_or(
        cross(geometric_normal, tangent), tangent_axis) * tangent_sign;
    const float4 sampled_normal = normal_texture.Sample(
        normal_sampler, input.normal_uv);
    float3 tangent_normal = float3(
        mapped_channel(sampled_normal, input.channel_mapping_a.w),
        mapped_channel(sampled_normal, input.channel_mapping_b.x),
        mapped_channel(sampled_normal, input.channel_mapping_b.y)) * 2.0 - 1.0;
    // OpenGL Y+ sources arrive with reversed tangent handedness from the scene
    // adapter, so this DirectX Y- sample adjustment remains convention-correct.
    tangent_normal.y = -tangent_normal.y;
    tangent_normal.xy *= input.material_surface_factors.z;
    const float3 normal = normalize_or(
        tangent * tangent_normal.x + bitangent * tangent_normal.y +
        geometric_normal * tangent_normal.z, geometric_normal);
    const float3 view = normalize_or(
        input.camera_clearcoat.xyz - input.world_position, geometric_normal);
    const float3 light_direction = normalize_or(
        light_direction_intensity.xyz, geometric_normal);
    const float n_dot_l = max(dot(normal, light_direction), 0.0);
    const float n_dot_v = max(dot(normal, view), 0.0);
    const float4 sampled_base_raw = base_color_texture.Sample(
        base_color_sampler, input.uv);
    const float4 sampled_base = float4(
        mapped_channel(sampled_base_raw, input.material_flags.y),
        mapped_channel(sampled_base_raw, input.material_flags.z),
        mapped_channel(sampled_base_raw, input.material_flags.w),
        mapped_channel(sampled_base_raw, input.channel_mapping_a.x));
    const float3 base_color = sampled_base.rgb * base_color_factor.rgb *
        input.vertex_color.rgb;
    const float4 sampled_metallic_roughness =
        metallic_roughness_texture.Sample(metallic_roughness_sampler,
            input.metallic_roughness_uv);
    const float metallic = saturate(input.material_surface_factors.x *
        mapped_channel(sampled_metallic_roughness, input.channel_mapping_a.y));
    const float roughness = saturate(input.material_surface_factors.y *
        mapped_channel(sampled_metallic_roughness, input.channel_mapping_a.z));
    // z is a scene-only lobe factor in the spare channel-mapping payload.
    const float sheen_weight = saturate(input.channel_mapping_c.z) * (1.0 - metallic);
    const float dielectric_f0 = saturate(emissive_radiance_and_f0.w);
    const float clearcoat = saturate(input.camera_clearcoat.w);
    const float coat_roughness = saturate(clearcoat_roughness);
    const float3 f0 = lerp(
        float3(dielectric_f0, dielectric_f0, dielectric_f0), base_color, metallic);
    const float3 half_vector = normalize_or(view + light_direction, normal);
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
    const float clearcoat_distribution = distribution_ggx(n_dot_h, coat_roughness);
    const float clearcoat_geometry =
        geometry_schlick_ggx(n_dot_l, coat_roughness) *
        geometry_schlick_ggx(n_dot_v, coat_roughness);
    const float3 clearcoat_specular = clearcoat * clearcoat_fresnel *
        clearcoat_distribution * clearcoat_geometry /
        max(4.0 * n_dot_l * n_dot_v, 1e-5);
    const float shadow = shadow_visibility(input.light_position, normal, light_direction);
    const bool has_occlusion_map = input.material_flags.x > 0.5;
    const float occlusion_parameter = input.material_surface_factors.w;
    const float occlusion = has_occlusion_map
        ? lerp(
            1.0,
            saturate(mapped_channel(occlusion_texture.Sample(occlusion_sampler,
                input.occlusion_uv), input.channel_mapping_b.z)),
            saturate(occlusion_parameter))
        : saturate(occlusion_parameter);
    const float3 sheen = base_color * (sheen_weight * base_attenuation *
        sheen_charlie_neubelt(roughness, n_dot_l, n_dot_v, n_dot_h));
    const float3 direct = ((diffuse + specular) * (1.0 - sheen_weight) +
        sheen + clearcoat_specular) *
        (n_dot_l * light_radiance * shadow);
    uint local_light_width = 0U;
    uint local_light_height = 0U;
    authored_local_lights.GetDimensions(local_light_width, local_light_height);
    const uint local_light_count = local_light_width >= 4U
        ? min(local_light_height, 64U) : 0U;
    float3 local_direct = 0.0;
    [loop]
    for (uint light_index = 0U; light_index < local_light_count; ++light_index)
    {
        const float4 position_range = authored_local_lights.Load(
            int3(0, int(light_index), 0));
        if (!all(isfinite(position_range)) || position_range.w <= 0.0)
            continue;
        const float3 to_light = position_range.xyz - input.world_position;
        const float distance_to_light = length(to_light);
        if (!isfinite(distance_to_light) || distance_to_light <= 1e-5 ||
            distance_to_light >= position_range.w)
            continue;
        const float3 local_light_direction = to_light / distance_to_light;
        const float local_n_dot_l = max(dot(normal, local_light_direction), 0.0);
        if (local_n_dot_l <= 0.0)
            continue;

        const float4 direction_kind = authored_local_lights.Load(
            int3(1, int(light_index), 0));
        const float4 radiance_inner = authored_local_lights.Load(
            int3(2, int(light_index), 0));
        const float4 outer_cone = authored_local_lights.Load(
            int3(3, int(light_index), 0));
        if (!all(isfinite(direction_kind)) || !all(isfinite(radiance_inner)) ||
            !all(isfinite(outer_cone)))
            continue;
        float cone_attenuation = 1.0;
        if (direction_kind.w > 0.5)
        {
            const float emitter_cosine = dot(
                normalize_or(direction_kind.xyz, normal), -local_light_direction);
            cone_attenuation = abs(radiance_inner.w - outer_cone.x) <= 1e-6
                ? (emitter_cosine >= radiance_inner.w ? 1.0 : 0.0)
                : smoothstep(outer_cone.x, radiance_inner.w, emitter_cosine);
        }
        if (cone_attenuation <= 0.0)
            continue;

        const float local_shadow = outer_cone.y > 0.5
            ? shadow_visibility(
                input.light_position, normal, local_light_direction)
            : 1.0;

        const float range_ratio = distance_to_light / position_range.w;
        const float range_ratio_squared = range_ratio * range_ratio;
        const float range_fade = max(
            1.0 - range_ratio_squared * range_ratio_squared, 0.0);
        // Keep the near field finite with a shared 1 cm minimum distance.
        const float inverse_square_distance = max(
            distance_to_light * distance_to_light, 1.0e-4);
        const float range_attenuation =
            range_fade * range_fade / inverse_square_distance;
        const float3 local_half_vector = normalize_or(view + local_light_direction, normal);
        const float local_n_dot_v = n_dot_v;
        const float local_n_dot_h = max(dot(normal, local_half_vector), 0.0);
        const float local_v_dot_h = max(dot(view, local_half_vector), 0.0);
        const float3 local_fresnel = fresnel_schlick(local_v_dot_h, f0);
        const float local_coat_fresnel =
            0.04 + 0.96 * pow(1.0 - local_v_dot_h, 5.0);
        const float local_base_attenuation =
            saturate(1.0 - clearcoat * local_coat_fresnel);
        const float local_distribution = distribution_ggx(
            local_n_dot_h, roughness);
        const float local_geometry =
            geometry_schlick_ggx(local_n_dot_l, roughness) *
            geometry_schlick_ggx(local_n_dot_v, roughness);
        const float3 local_specular = local_fresnel * local_distribution *
            local_geometry * local_base_attenuation /
            max(4.0 * local_n_dot_l * local_n_dot_v, 1e-5);
        const float3 local_diffuse_weight =
            (1.0 - local_fresnel) * (1.0 - metallic);
        const float3 local_diffuse = local_diffuse_weight * base_color *
            local_base_attenuation / 3.14159265;
        const float local_coat_distribution = distribution_ggx(
            local_n_dot_h, coat_roughness);
        const float local_coat_geometry =
            geometry_schlick_ggx(local_n_dot_l, coat_roughness) *
            geometry_schlick_ggx(local_n_dot_v, coat_roughness);
        const float3 local_coat_specular = clearcoat * local_coat_fresnel *
            local_coat_distribution * local_coat_geometry /
            max(4.0 * local_n_dot_l * local_n_dot_v, 1e-5);
        const float3 local_sheen = base_color *
            (sheen_weight * local_base_attenuation * sheen_charlie_neubelt(
                roughness, local_n_dot_l, local_n_dot_v, local_n_dot_h));
        local_direct += ((local_diffuse + local_specular) * (1.0 - sheen_weight) +
            local_sheen + local_coat_specular) *
            (local_n_dot_l * radiance_inner.rgb * range_attenuation *
                cone_attenuation * local_shadow);
    }
    const bool has_diffuse_irradiance =
        scene_environment.Load(int3(0, 0, 0)).a > 0.5;
    const float3 ambient = has_diffuse_irradiance
        ? evaluate_diffuse_irradiance_over_pi(normal) * base_color * diffuse_weight *
            (base_attenuation * occlusion)
        : base_color * (0.03 * occlusion);
    uint environment_width = 0U;
    uint environment_height = 0U;
    uint environment_mip_levels = 0U;
    scene_environment.GetDimensions(
        0U, environment_width, environment_height, environment_mip_levels);
    const bool has_specular_environment = environment_width >= 3U &&
        environment_height >= 3U && environment_mip_levels > 1U &&
        scene_environment.Load(int3(2, 2, 0)).a > 0.5;
    float3 environment_specular = 0.0;
    float3 environment_clearcoat = 0.0;
    if (has_specular_environment)
    {
        const float3 reflected_direction = reflect(-view, normal);
        const float2 specular_brdf = sample_split_sum_brdf(n_dot_v, roughness);
        environment_specular = sample_specular_environment(
            reflected_direction, roughness, environment_mip_levels) *
            (f0 * specular_brdf.x + specular_brdf.y) * base_attenuation;
        if (clearcoat > 0.0)
        {
            const float2 clearcoat_brdf =
                sample_split_sum_brdf(n_dot_v, coat_roughness);
            environment_clearcoat = sample_specular_environment(
                reflected_direction, coat_roughness, environment_mip_levels) *
                (clearcoat * (0.04 * clearcoat_brdf.x + clearcoat_brdf.y));
        }
    }
    const float4 sampled_emissive = emissive_texture.Sample(emissive_sampler,
        input.emissive_uv);
    const float3 emissive = float3(
        mapped_channel(sampled_emissive, input.channel_mapping_b.w),
        mapped_channel(sampled_emissive, input.channel_mapping_c.x),
        mapped_channel(sampled_emissive, input.channel_mapping_c.y)) *
        emissive_radiance_and_f0.rgb;
    const float alpha = sampled_base.a * base_color_factor.a *
        input.vertex_color.a;
    // A negative light-direction w encodes masked alpha and its cutoff. The
    // positive path remains opaque/blended-compatible for the current pass.
    if (light_direction_intensity.w < 0.0 &&
        alpha < -light_direction_intensity.w) discard;
    const float3 hdr = max(direct + local_direct + ambient + environment_specular +
        environment_clearcoat + emissive, 0.0);
    return float4(hdr, alpha);
}
