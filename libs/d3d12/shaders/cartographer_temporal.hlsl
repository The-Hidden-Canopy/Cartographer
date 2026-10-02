// Cartographer temporal resolve shader preparation.

cbuffer TemporalConstants : register(b0)
{
    float4 feedback_and_clamp;
    float4 jitter_and_inverse_extent;
    uint4 history_flags; // x = valid, y = camera cut, z = source revision low
};

Texture2D<float4> current_color : register(t0);
Texture2D<float4> history_color : register(t1);
Texture2D<float2> motion_vectors : register(t2);
RWTexture2D<float4> resolved_color : register(u0);

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
    const float4 current = current_color.Load(int3(pixel, 0));
    if (history_flags.x == 0 || history_flags.y != 0)
    {
        resolved_color[pixel] = current;
        return;
    }

    const float2 motion = motion_vectors.Load(int3(pixel, 0));
    const int2 history_pixel = clamp(
        pixel - int2(round(motion)), int2(0, 0), int2(width - 1, height - 1));
    const float4 history = history_color.Load(int3(history_pixel, 0));
    const float3 lower = min(current.rgb, history.rgb);
    const float3 upper = max(current.rgb, history.rgb);
    const float3 clamped_history = clamp(history.rgb, lower, upper);
    const float feedback = saturate(feedback_and_clamp.x);
    resolved_color[pixel] = float4(
        lerp(current.rgb, clamped_history, feedback), current.a);
}
