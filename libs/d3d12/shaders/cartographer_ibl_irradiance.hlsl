// Bounded equirectangular irradiance quadrature for the D3D12 reference path.
// This pass evaluates one +Z normal and uses pixel-center hemisphere samples;
// it is not a cubemap convolution or a general environment prefilter.

Texture2D<float4> environment_map : register(t0);
RWTexture2D<float4> irradiance_output : register(u0);

[numthreads(1, 1, 1)]
void CSMain(uint3 dispatch_id : SV_DISPATCHTHREADID)
{
    uint width;
    uint height;
    environment_map.GetDimensions(width, height);
    float3 weighted_radiance = 0.0;
    float total_weight = 0.0;
    const float pi = 3.14159265358979323846;
    for (uint y = 0; y < height; ++y)
    {
        const float theta = pi * (float(y) + 0.5) / float(height);
        const float weight = max(cos(theta), 0.0) * sin(theta);
        for (uint x = 0; x < width; ++x)
        {
            weighted_radiance += environment_map.Load(int3(x, y, 0)).rgb * weight;
            total_weight += weight;
        }
    }
    const float3 irradiance = total_weight > 0.0
        ? weighted_radiance * (pi / total_weight)
        : 0.0;
    irradiance_output[uint2(0, 0)] = float4(irradiance, 1.0);
}
