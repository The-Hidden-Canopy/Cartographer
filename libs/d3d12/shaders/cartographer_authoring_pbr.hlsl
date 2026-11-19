// Cartographer forward PBR shader preparation.
//
// The D3D12 executor exposes b0 as root constants and a bounded t0..t4/s0
// material table. The five texture slots intentionally mirror StandardMaterial
// instead of hiding texture selection in shader-specific conventions.

cbuffer PushConstants : register(b0)
{
    float4x4 view_projection;
    float4x4 model;
    // xyz is camera position (or authored orthographic view direction).
    // w is reserved for authored view/identity metadata, never exposure.
    float4 camera_position_and_view;
    float4 base_color_factor;
    float4 emissive_radiance_and_f0;
    float4 surface_factors;
    float4 light_direction_intensity;
#ifndef CARTO_AUTHORING_VIEWPORT
    float4 light_radiance_range;
#endif
    // The compact float2 keeps this descriptor-heavy root signature at the
    // D3D12 64-DWORD limit.
#ifndef CARTO_AUTHORING_SURFACE
    float2 clearcoat_factors;
#endif
};

#ifndef CARTO_AUTHORING_SURFACE
Texture2D<float4> base_color_texture : register(t0);
Texture2D<float4> metallic_roughness_texture : register(t1);
Texture2D<float4> normal_texture : register(t2);
Texture2D<float4> occlusion_texture : register(t3);
Texture2D<float4> emissive_texture : register(t4);
#endif
SamplerState linear_clamp_sampler : register(s0);
#ifdef CARTO_AUTHORING_VIEWPORT
// Immutable small lighting packet and spatial depth map. Keeping light-space
// data out of per-draw constants leaves the full surface root signature <=64.
Texture2D<float4> authoring_lighting : register(t6);
Texture2D<float> authoring_shadow : register(t7);
float4 light_word(uint x) { return authoring_lighting.Load(int3(x,0,0)); }
float shadow_visibility(float3 world,float3 normal,float3 light) {
    float4 settings=light_word(5);
    if(settings.x==0) return 1;
    float4 p=float4(world,1);
    float3 q=float3(dot(light_word(0),p),dot(light_word(1),p),dot(light_word(2),p));
    float2 uv=float2(q.x*.5+.5,.5-q.y*.5);
    if(any(uv<0) || any(uv>1) || q.z<0 || q.z>1) return 1;
    uint width,height;authoring_shadow.GetDimensions(width,height);
    int2 center=int2(floor(uv*float2(width-1,height-1)));
    if(!all(isfinite(normal)) || dot(normal,normal)<=0) return -1;
    normal=normalize(normal);
    float3 plane=float3(dot(normal,normalize(light_word(0).xyz)),
        dot(normal,normalize(light_word(1).xyz)),dot(normal,-normalize(light_word(2).xyz)));
    float2 gradient=abs(plane.z)>1e-5 ? float2(plane.x,-plane.y)/plane.z*light_word(6).y : 0;
    float bias=settings.z+settings.w*(1-saturate(abs(dot(normal,light))));
    int radius=int(light_word(6).x);float visible=0,taps=0;
    [loop] for(int y=-radius;y<=radius;++y) [loop] for(int x=-radius;x<=radius;++x) {
        int2 tap=clamp(center+int2(x,y),int2(0,0),int2(width-1,height-1));
        float depth=authoring_shadow.Load(int3(tap,0));
        if(!isfinite(depth) || depth<0 || depth>1) return -1;
        float receiver=q.z+dot(gradient,(float2(tap)+.5)/float2(width,height)-uv);
        visible+=(receiver<0 || receiver>1 || receiver-bias<=depth)?1:0;++taps;
    }
    return visible/taps;
}
#endif

struct VertexInput
{
    float4 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    float4 tangent : TANGENT;
#ifdef CARTO_AUTHORING_VIEWPORT
    float4 color : COLOR0;
#endif
};

struct VertexOutput
{
    float4 position : SV_POSITION;
    float3 world_position : TEXCOORD0;
    float3 world_normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
    float3 world_tangent : TEXCOORD3;
    float tangent_sign : TEXCOORD4;
    nointerpolation float orientation : TEXCOORD5;
#ifdef CARTO_AUTHORING_VIEWPORT
    float4 color : COLOR0;
#endif
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
    output.world_normal = normalize(orientation * (
        cross(c1, c2) * input.normal.x + cross(c2, c0) * input.normal.y +
        cross(c0, c1) * input.normal.z));
    output.uv = input.uv;
    output.world_tangent = normalize(mul((float3x3)model, input.tangent.xyz));
    output.tangent_sign = input.tangent.w * orientation;
    output.orientation = orientation;
#ifdef CARTO_AUTHORING_VIEWPORT
    output.color = input.color;
#endif
    return output;
}

float distribution_ggx(float3 normal, float3 half_vector, float roughness)
{
    const float alpha = max(0.045 * 0.045, roughness * roughness);
    const float alpha_squared = alpha * alpha;
    const float cosine = saturate(dot(normal, half_vector));
    const float cosine_squared = cosine * cosine;
    // Both vectors are normalized. Preserve the small angle at a smooth peak
    // without subtracting a rounded cosine squared from one.
    const float3 sine = cross(normal, half_vector);
    // Preserve the original clamped back-hemisphere value, including -parallel.
    const float sine_squared = cosine > 0.0 ? dot(sine, sine) : 1.0;
    precise float denominator = sine_squared + alpha_squared * cosine_squared;
    return alpha_squared / (3.14159265 * denominator * denominator);
}

float geometry_schlick_ggx(float n_dot_x, float roughness)
{
    const float alpha = max(0.045 * 0.045, roughness * roughness);
    const float k = ((alpha + 1.0) * (alpha + 1.0)) / 8.0;
    return n_dot_x / max(n_dot_x * (1.0 - k) + k, 1e-12);
}

float3 fresnel_schlick(float cos_theta, float3 f0)
{
    return f0 + (1.0 - f0) * pow(1.0 - cos_theta, 5.0);
}

#ifdef CARTO_AUTHORING_SURFACE
// Same shortest-rotation frame transport used by the scene renderer. Directly
// projecting the old tangent can collapse when a normal map points along it.
float3 authoring_transport_tangent(float3 geometric, float3 tangent, float3 shading)
{
    float3 axis = cross(geometric, shading);
    const float sine_squared = dot(axis, axis);
    if (sine_squared > 1e-12) {
        const float sine = sqrt(sine_squared);
        axis /= sine;
        const float cosine = clamp(dot(geometric, shading), -1.0, 1.0);
        tangent = tangent*cosine + cross(axis,tangent)*sine +
                  axis*(dot(axis,tangent)*(1.0-cosine));
    }
    tangent -= shading*dot(shading,tangent);
    const float length_squared = dot(tangent,tangent);
    if (length_squared > 1e-12) return tangent*rsqrt(length_squared);
    const float3 reference = abs(shading.z) < .999 ? float3(0,0,1) : float3(0,1,0);
    return normalize(cross(reference,shading));
}
#endif

float4 PSMain(VertexOutput input, uint primitive : SV_PrimitiveID,
              bool front_facing : SV_IsFrontFace) : SV_TARGET
{
    const float3 geometric_normal = normalize(input.world_normal);
#ifdef CARTO_AUTHORING_VIEWPORT
    // Derivatives describe the actual receiver plane even with custom corner
    // normals/normal mapping. Tap correction must not follow shading normals.
    const float3 receiver_plane=cross(ddx(input.world_position),ddy(input.world_position));
#endif
#ifdef CARTO_AUTHORING_SURFACE
    const EvaluatedMaterial material=evaluate_material(input.uv,base_color_factor,
        emissive_radiance_and_f0.rgb,surface_factors,emissive_radiance_and_f0.w);
    if(!material.valid) return float4(0,0,0,-((abs(camera_position_and_view.w)-1)*65536+primitive+1));
    float3 normal=geometric_normal;
    if(material.normal_mapped) {
        const float3 tangent=normalize(input.world_tangent-geometric_normal*dot(geometric_normal,input.world_tangent));
        const float3 bitangent=cross(geometric_normal,tangent)*input.tangent_sign;
        normal=normalize(tangent*material.tangent_normal.x+bitangent*material.tangent_normal.y+geometric_normal*material.tangent_normal.z);
    }
    const float3 view=camera_position_and_view.w<0 ? normalize(camera_position_and_view.xyz)
        : normalize(camera_position_and_view.xyz-input.world_position);
    if(!all(isfinite(normal)) || !all(isfinite(view)))
        return float4(0,0,0,-((abs(camera_position_and_view.w)-1)*65536+primitive+1));
#elif defined(CARTO_AUTHORING_VIEWPORT)
    // G1 constant material path: no normal/UV texture is claimed executable.
    float3 normal = geometric_normal;
    // Negative w selects an orthographic view direction in xyz; nonnegative
    // w selects a perspective camera position. Fused draws also pack their
    // ordinal in abs(w). Exposure is exclusively display-owned.
    const float3 view = camera_position_and_view.w < 0.0
        ? normalize(camera_position_and_view.xyz)
        : normalize(camera_position_and_view.xyz - input.world_position);
#else
    const float3 tangent = normalize(
        input.world_tangent - geometric_normal * dot(geometric_normal, input.world_tangent));
    const float3 bitangent = normalize(cross(geometric_normal, tangent)) * input.tangent_sign;
    float3 tangent_normal = normal_texture.Sample(linear_clamp_sampler, input.uv).xyz * 2.0 - 1.0;
    // The bounded first material contract uses the DirectX Y- convention.
    tangent_normal.y = -tangent_normal.y;
    tangent_normal.xy *= surface_factors.z;
    float3 normal = normalize(
        tangent * tangent_normal.x + bitangent * tangent_normal.y +
        geometric_normal * tangent_normal.z);
    const float3 view = normalize(camera_position_and_view.xyz - input.world_position);
#endif
    // Authoring uses two-sided rasterization. Match glTF by reversing the
    // complete shading normal, including normal maps, on the back side.
    if (front_facing == (input.orientation < 0.0)) normal = -normal;
    const float3 light_direction = normalize(light_direction_intensity.xyz);
    const float n_dot_l = max(dot(normal, light_direction), 0.0);
    const float n_dot_v = max(dot(normal, view), 0.0);
#ifdef CARTO_AUTHORING_SURFACE
    const float4 sampled_base=1;
    const float3 base_color=material.base.rgb * input.color.rgb;
    const float metallic=material.metallic,roughness=material.roughness;
#else
#ifdef CARTO_FUSED_IDENTITY
    const float4 sampled_base = 1.0;
#else
    const float4 sampled_base = base_color_texture.Sample(linear_clamp_sampler, input.uv);
#endif
#ifdef CARTO_AUTHORING_VIEWPORT
    const float3 base_color = sampled_base.rgb * base_color_factor.rgb * input.color.rgb;
#else
    const float3 base_color = sampled_base.rgb * base_color_factor.rgb;
#endif
#ifdef CARTO_FUSED_IDENTITY
    const float metallic = saturate(surface_factors.x);
    const float roughness = saturate(surface_factors.y);
#else
    const float metallic = saturate(surface_factors.x * metallic_roughness_texture.Sample(linear_clamp_sampler, input.uv).b);
    const float roughness = saturate(surface_factors.y * metallic_roughness_texture.Sample(linear_clamp_sampler, input.uv).g);
#endif
#endif
    const float dielectric_f0 = saturate(emissive_radiance_and_f0.w);
#ifdef CARTO_AUTHORING_SURFACE
    const float clearcoat=material.coat,clearcoat_roughness=material.coat_roughness;
    const float transmission=material.transmission*(1-metallic);
    const float anisotropy=material.anisotropy;
#else
    const float clearcoat = saturate(clearcoat_factors.x);
    const float clearcoat_roughness = saturate(clearcoat_factors.y);
    const float transmission = 0.0;
    const float anisotropy = 0.0;
#endif
    const float3 f0 = lerp(
        float3(dielectric_f0, dielectric_f0, dielectric_f0), base_color, metallic);
    const float3 sum_direction = view + light_direction;
    const float3 half_vector = dot(sum_direction, sum_direction) > 1e-12
        ? normalize(sum_direction) : normal;
    const float n_dot_h = max(dot(normal, half_vector), 0.0);
    const float v_dot_h = max(dot(view, half_vector), 0.0);
    const float3 fresnel = fresnel_schlick(v_dot_h, f0);
    const float clearcoat_fresnel = 0.04 + 0.96 * pow(1.0 - v_dot_h, 5.0);
    const float base_attenuation = saturate(1.0 - clearcoat * clearcoat_fresnel);
    float distribution = distribution_ggx(normal, half_vector, roughness);
    float3 specular_normal = normal;
#ifdef CARTO_AUTHORING_SURFACE
    if(anisotropy>0) {
        // glTF KHR_materials_anisotropy: alpha_t = mix(alpha, 1, s^2), alpha_b = alpha,
        // along the mesh tangent rotated by the authored angle.
        const float3 geometric_tangent=normalize(input.world_tangent-
            geometric_normal*dot(geometric_normal,input.world_tangent));
        const float3 t0=authoring_transport_tangent(geometric_normal,geometric_tangent,normal);
        float3 b0=cross(normal,t0)*input.tangent_sign;
        const float c=cos(material.anisotropy_rotation),s=sin(material.anisotropy_rotation);
        const float3 t=normalize(t0*c+b0*s),b=normalize(cross(normal,t))*input.tangent_sign;
        const float alpha=max(roughness*roughness,0.045*0.045);
        const float at=lerp(alpha,1.0,anisotropy*anisotropy),ab=alpha;
        const float3 h=float3(dot(half_vector,t),dot(half_vector,b),n_dot_h);
        const float d=h.x*h.x/(at*at)+h.y*h.y/(ab*ab)+h.z*h.z;
        distribution=1.0/(3.14159265*at*ab*d*d);
        // Bent reflection normal for the environment (stretched highlight).
        const float3 anisotropic_tangent=cross(b,view);
        const float3 anisotropic_normal=cross(anisotropic_tangent,b);
        const float3 bent=lerp(normal,anisotropic_normal,anisotropy*saturate(1.0-roughness*0.5));
        if(dot(bent,bent)>1e-12) specular_normal=normalize(bent);
    }
#endif
    const float geometry = geometry_schlick_ggx(n_dot_l, roughness) *
        geometry_schlick_ggx(n_dot_v, roughness);
    const float3 specular = fresnel * distribution * geometry * base_attenuation /
        max(4.0 * n_dot_l * n_dot_v, 1e-12);
    const float3 diffuse_weight = (1.0 - fresnel) * (1.0 - metallic) * (1.0 - transmission);
    const float3 diffuse = diffuse_weight * base_color * base_attenuation / 3.14159265;
    const float clearcoat_distribution = distribution_ggx(
        normal, half_vector, clearcoat_roughness);
    const float clearcoat_geometry =
        geometry_schlick_ggx(n_dot_l, clearcoat_roughness) *
        geometry_schlick_ggx(n_dot_v, clearcoat_roughness);
    const float3 clearcoat_specular = clearcoat * clearcoat_fresnel *
        clearcoat_distribution * clearcoat_geometry /
        max(4.0 * n_dot_l * n_dot_v, 1e-12);
#ifdef CARTO_AUTHORING_VIEWPORT
    const float4 light_radiance_range=light_word(4);
    const float shadow=(n_dot_l>0 && n_dot_v>0)
        ? shadow_visibility(input.world_position,receiver_plane,light_direction) : 1;
    if(shadow<0) return float4(0,0,0,-((abs(camera_position_and_view.w)-1)*65536+primitive+1));
#else
    const float shadow = saturate(light_radiance_range.w);
#endif
    float3 direct = (n_dot_l > 0.0 && n_dot_v > 0.0)
        ? (diffuse + specular + clearcoat_specular) * (n_dot_l * light_radiance_range.rgb * shadow)
        : 0.0;
#ifdef CARTO_AUTHORING_VIEWPORT
    // The primary source above owns the single shadow map. Additional active
    // directional sources are admitted only when they do not need another map.
    [unroll] for (uint light_index = 0; light_index < 3; ++light_index) {
        const float4 packed_direction = light_word(8 + 2 * light_index);
        if (packed_direction.w == 0) continue;
        const float3 extra_light = normalize(packed_direction.xyz);
        const float extra_n_dot_l = max(dot(normal, extra_light), 0.0);
        if (extra_n_dot_l <= 0 || n_dot_v <= 0) continue;
        const float3 extra_sum = view + extra_light;
        const float3 extra_half = dot(extra_sum, extra_sum) > 1e-12 ? normalize(extra_sum) : normal;
        const float extra_n_dot_h = max(dot(normal, extra_half), 0.0);
        const float extra_v_dot_h = max(dot(view, extra_half), 0.0);
        const float3 extra_fresnel = fresnel_schlick(extra_v_dot_h, f0);
        const float extra_coat_fresnel = 0.04 + 0.96 * pow(1.0 - extra_v_dot_h, 5.0);
        const float extra_attenuation = saturate(1.0 - clearcoat * extra_coat_fresnel);
        float extra_distribution = distribution_ggx(normal, extra_half, roughness);
#ifdef CARTO_AUTHORING_SURFACE
        if (anisotropy > 0) {
            const float3 geometric_tangent = normalize(input.world_tangent -
                geometric_normal * dot(geometric_normal, input.world_tangent));
            const float3 t0 = authoring_transport_tangent(geometric_normal, geometric_tangent, normal);
            const float3 b0 = cross(normal, t0) * input.tangent_sign;
            const float c = cos(material.anisotropy_rotation), s = sin(material.anisotropy_rotation);
            const float3 t = normalize(t0 * c + b0 * s), b = normalize(cross(normal, t)) * input.tangent_sign;
            const float alpha = max(roughness * roughness, 0.045 * 0.045);
            const float at = lerp(alpha, 1.0, anisotropy * anisotropy), ab = alpha;
            const float3 h = float3(dot(extra_half, t), dot(extra_half, b), extra_n_dot_h);
            const float d = h.x*h.x/(at*at) + h.y*h.y/(ab*ab) + h.z*h.z;
            extra_distribution = 1.0/(3.14159265*at*ab*d*d);
        }
#endif
        const float extra_geometry = geometry_schlick_ggx(extra_n_dot_l, roughness) *
            geometry_schlick_ggx(n_dot_v, roughness);
        const float3 extra_specular = extra_fresnel * extra_distribution * extra_geometry *
            extra_attenuation / max(4.0 * extra_n_dot_l * n_dot_v, 1e-12);
        const float3 extra_diffuse = (1.0-extra_fresnel) * (1.0-metallic) * (1.0-transmission) *
            base_color * extra_attenuation / 3.14159265;
        const float extra_coat_distribution = distribution_ggx(normal, extra_half, clearcoat_roughness);
        const float extra_coat_geometry = geometry_schlick_ggx(extra_n_dot_l, clearcoat_roughness) *
            geometry_schlick_ggx(n_dot_v, clearcoat_roughness);
        const float3 extra_coat = clearcoat * extra_coat_fresnel * extra_coat_distribution *
            extra_coat_geometry / max(4.0 * extra_n_dot_l * n_dot_v, 1e-12);
        direct += (extra_diffuse + extra_specular + extra_coat) *
            (extra_n_dot_l * light_word(9 + 2 * light_index).rgb);
    }
#endif
#ifdef CARTO_AUTHORING_SURFACE
    const float3 emissive=material.emission;
    const float alpha=material.base.a * input.color.a;
#else
#ifdef CARTO_FUSED_IDENTITY
    const float3 emissive = emissive_radiance_and_f0.rgb;
#else
    const float3 emissive = emissive_texture.Sample(linear_clamp_sampler, input.uv).rgb * emissive_radiance_and_f0.rgb;
#endif
#ifdef CARTO_AUTHORING_VIEWPORT
    const float alpha = sampled_base.a * base_color_factor.a * input.color.a;
#else
    const float alpha = sampled_base.a * base_color_factor.a;
#endif
#endif
    // A negative light-direction w encodes masked alpha and its cutoff. The
    // positive path remains opaque/blended-compatible for the current pass.
    if (light_direction_intensity.w < 0.0 &&
        alpha < -light_direction_intensity.w) discard;
    // Direct-light material evaluation always produces unexposed linear HDR.
    // Environment light is not replaced by an ambient color/AO surrogate.
    // Camera exposure, tone mapping and encoding belong to the display pass.
    float3 hdr = direct + emissive;
#ifdef CARTO_AUTHORING_VIEWPORT
#ifdef CARTO_AUTHORING_SURFACE
    const float environment_ao=material.ao;
#else
    const float environment_ao=1;
#endif
    hdr+=environment_evaluate(specular_normal,view,base_color*(1.0-transmission),f0,roughness,metallic,clearcoat,
        clearcoat_roughness,environment_ao,light_word(7));
#ifdef CARTO_AUTHORING_SURFACE
    if(transmission>0) {
        // Glass preview: the refracted environment, tinted by base colour and
        // by Beer-Lambert absorption over the authored wall thickness, behind
        // the dielectric Fresnel. Scene geometry behind the glass is not
        // refracted in this raster preview.
        const float eta=1.0/material.ior;
        float3 refracted=refract(-view,normal,eta);
        if(dot(refracted,refracted)<1e-8) refracted=reflect(-view,normal);
        const float cos_t=max(abs(dot(refracted,normal)),0.2);
        const float path=material.thickness/cos_t;
        const float3 absorption=material.attenuation_distance>0
            ? pow(max(material.attenuation_color,1e-4),path/material.attenuation_distance) : 1.0;
        // IOR controls the bend; the material's independent dielectric F0
        // controls reflection. Use the same F0 for the complementary
        // transmitted share so reflection and transmission cannot both take
        // the same energy when those authored values differ.
        const float dielectric=dielectric_f0+(1-dielectric_f0)*pow(1-saturate(dot(normal,view)),5.0);
        hdr+=environment_direction(refracted,roughness,light_word(7))*base_color*absorption*
            (transmission*(1.0-dielectric)*environment_ao);
    }
#endif
#endif
#ifdef CARTO_AUTHORING_SURFACE
    if(!all(isfinite(hdr))) return float4(0,0,0,-((abs(camera_position_and_view.w)-1)*65536+primitive+1));
#endif
#ifdef CARTO_FUSED_IDENTITY
    // Preserve integer identity bits in a positive normal float, rather than
    // converting a >24-bit ordinal numerically. The bias avoids subnormal FTZ.
    const uint draw_ordinal = (uint)(abs(camera_position_and_view.w)-1.0);
    const uint identity = draw_ordinal*65536u + primitive + 1u;
    const float packed_identity = asfloat(0x00800000u + identity);
    return float4(hdr, packed_identity);
#else
    return float4(hdr, alpha);
#endif
}

#ifdef CARTO_AUTHORING_VIEWPORT
float4 ShadowMain(VertexOutput input) : SV_TARGET {
    if(light_direction_intensity.w<0) {
#ifdef CARTO_AUTHORING_SURFACE
        const EvaluatedMaterial m=evaluate_material(input.uv,base_color_factor,
            emissive_radiance_and_f0.rgb,surface_factors,emissive_radiance_and_f0.w);
        if(!m.valid) return float4(-1,0,0,1);
        const float alpha=m.base.a * input.color.a;
#elif defined(CARTO_FUSED_IDENTITY)
        const float alpha=base_color_factor.a * input.color.a;
#else
        const float alpha=base_color_texture.Sample(linear_clamp_sampler,input.uv).a*base_color_factor.a*input.color.a;
#endif
        if(alpha < -light_direction_intensity.w) discard;
    }
    // SV_POSITION.z is already the interpolated viewport depth. Dividing by
    // SV_POSITION.w again would corrupt perspective shadow maps.
    return float4(input.position.z,0,0,1);
}
float4 IdentityMain(VertexOutput input, uint primitive : SV_PrimitiveID) : SV_TARGET
{
    if (light_direction_intensity.w < 0.0 &&
        base_color_factor.a * input.color.a < -light_direction_intensity.w) discard;
    // Bounded draw/primitive ordinals are exactly representable in float32.
    // The host resolves them to full-width authored object and face IDs.
    return float4(camera_position_and_view.w + 1.0, primitive + 1.0, 0.0, 1.0);
}
#endif
