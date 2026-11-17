// Authored KHR anisotropy direct-light kernel, matching authored_anisotropy.hpp.
// Caller validates roughness/strength in [0,1], finite rotation and unit local
// directions in the authored handed (T,B,N) frame. Reduce rotation modulo
// 2*pi in double precision before narrowing to float. No maps, Fresnel or IBL.
// alpha floor 1e-4 is numerical regularization, not a normative glTF minimum.
float3 carto_authored_anisotropy(float roughness, float strength, float rotation,
    float3 view, float3 light)
{
    if (view.z <= 0.0 || light.z <= 0.0) return 0.0;
    view = normalize(view); light = normalize(light);
    float c = cos(rotation), s = sin(rotation);
    view = float3(c*view.x+s*view.y, -s*view.x+c*view.y, view.z);
    light = float3(c*light.x+s*light.y, -s*light.x+c*light.y, light.z);
    float ab = max(1.0e-4, roughness*roughness);
    float at = lerp(ab, 1.0, strength*strength);
    float3 sum = view + light;
    // Rescaling avoids a squared-length underflow near opposing grazing rays.
    float3 half_vector = normalize(sum / max(max(abs(sum.x),abs(sum.y)),abs(sum.z)));
    float3 h = float3(half_vector.x/at,half_vector.y/ab,half_vector.z);
    float ellipse = dot(h,h);
    float distribution = 1.0 / (3.141592653589793 * at*ab*ellipse*ellipse);
    float denominator = light.z*length(float3(at*view.x,ab*view.y,view.z)) +
        view.z*length(float3(at*light.x,ab*light.y,light.z));
    float visibility = denominator > 0.0 ? min(1.0,0.5/denominator) : 1.0;
    return float3(distribution,visibility,distribution*visibility);
}
