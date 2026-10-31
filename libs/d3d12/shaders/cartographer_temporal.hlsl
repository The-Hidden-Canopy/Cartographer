// Cartographer temporal resolve shader preparation.

cbuffer TemporalConstants : register(b0)
{
    float4 feedback_and_clamp;
    float4 jitter_and_inverse_extent;
    uint4 history_flags; // x = valid, y = camera cut, z = revision low, w = motion mode
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
    if (!all(isfinite(current.rgb)))
    {
        resolved_color[pixel] = current;
        return;
    }
    if (history_flags.x == 0 || history_flags.y != 0)
    {
        resolved_color[pixel] = current;
        return;
    }

    // history_flags.w: 1 means the caller explicitly asserted zero motion; 2
    // means the constant pixel offset is the complete motion field. Neither is
    // inferred from texture contents. These paths avoid a motion-texture load
    // when the caller supplied no per-pixel field.
    float4 history;
    if (history_flags.w == 1)
    {
        history = history_color.Load(int3(pixel, 0));
    }
    else
    {
        float2 motion = jitter_and_inverse_extent.xy;
        if (history_flags.w != 2)
        {
            motion += motion_vectors.Load(int3(pixel, 0));
        }
        if (!all(isfinite(motion)) || any(abs(motion) > float2(width, height)))
        {
            resolved_color[pixel] = current;
            return;
        }
        // Motion is the current-pixel-to-history-pixel offset documented by
        // SceneRenderTemporalInputs, so it is added in floating-point pixel
        // coordinates. Bilinear loads preserve subpixel jitter without a
        // sampler binding or clamping out-of-frame history to an edge.
        const float2 history_position = float2(pixel) + motion;
        const float2 last_pixel = float2(width - 1, height - 1);
        if (!all(isfinite(history_position)) ||
            any(history_position < float2(0.0, 0.0)) ||
            any(history_position > last_pixel))
        {
            resolved_color[pixel] = current;
            return;
        }
        const int2 history_pixel_00 = int2(floor(history_position));
        const int2 history_pixel_11 = min(
            history_pixel_00 + int2(1, 1), int2(width - 1, height - 1));
        const int2 history_pixel_10 =
            int2(history_pixel_11.x, history_pixel_00.y);
        const int2 history_pixel_01 =
            int2(history_pixel_00.x, history_pixel_11.y);
        const float2 history_fraction =
            history_position - float2(history_pixel_00);
        const float4 history_row_0 = lerp(
            history_color.Load(int3(history_pixel_00, 0)),
            history_color.Load(int3(history_pixel_10, 0)),
            history_fraction.x);
        const float4 history_row_1 = lerp(
            history_color.Load(int3(history_pixel_01, 0)),
            history_color.Load(int3(history_pixel_11, 0)),
            history_fraction.x);
        history = lerp(history_row_0, history_row_1, history_fraction.y);
    }
    if (!all(isfinite(history.rgb)))
    {
        resolved_color[pixel] = current;
        return;
    }
    // Clamp valid history to the finite current-frame 3x3 neighborhood. This
    // bounds stale edge/specular colors after disocclusion without replicating
    // edge texels outside the frame; out-of-frame reprojection returned above.
    float3 neighborhood_min = current.rgb;
    float3 neighborhood_max = current.rgb;
    [unroll]
    for (int offset_y = -1; offset_y <= 1; ++offset_y)
    {
        [unroll]
        for (int offset_x = -1; offset_x <= 1; ++offset_x)
        {
            const int2 neighbor_pixel = pixel + int2(offset_x, offset_y);
            if (any(neighbor_pixel < int2(0, 0)) ||
                any(neighbor_pixel >= int2(width, height)))
            {
                continue;
            }
            const float3 neighbor =
                current_color.Load(int3(neighbor_pixel, 0)).rgb;
            if (all(isfinite(neighbor)))
            {
                neighborhood_min = min(neighborhood_min, neighbor);
                neighborhood_max = max(neighborhood_max, neighbor);
            }
        }
    }
    const float3 bounded_history = clamp(
        history.rgb, neighborhood_min, neighborhood_max);
    const float feedback = saturate(feedback_and_clamp.x);
    resolved_color[pixel] = float4(
        lerp(current.rgb, bounded_history, feedback), current.a);
}
