// Shared linear-HDR temporal resolve: seven-SRV numeric fixtures or four-SRV
// packed native GPU geometry. Exposure and display encoding remain downstream.
cbuffer TemporalConstants : register(b0) {
    float4 feedback_depth_clip; // feedback, absolute depth, relative depth, clip expansion
    uint4 extent_flags; // width,height,matching-history-valid,camera-cut
};
Texture2D<float4> current_hdr : register(t0);
Texture2D<float4> history_hdr : register(t1);
#ifdef CARTO_TEMPORAL_PACKED
// 3x3 R32_UINT tiles: identity64x2, old sample float4, current linear depth.
Texture2D<uint> current_geometry : register(t2);
Texture2D<uint> history_geometry : register(t3);
uint geometry_word(bool prior,int2 p,int slot) {
    int2 q=p*3+int2(slot%3,slot/3);
    return prior?history_geometry.Load(int3(q,0)):current_geometry.Load(int3(q,0));
}
float4 previous_at(int2 p) {
    return asfloat(uint4(geometry_word(false,p,4),geometry_word(false,p,5),geometry_word(false,p,6),geometry_word(false,p,7)));
}
float depth_at(bool prior,int2 p){return asfloat(geometry_word(prior,p,8));}
uint4 identity_at(bool prior,int2 p) {
    return uint4(geometry_word(prior,p,0),geometry_word(prior,p,1),geometry_word(prior,p,2),geometry_word(prior,p,3));
}
#else
Texture2D<float4> previous_sample : register(t2);
Texture2D<float> current_depth : register(t3);
Texture2D<float> history_depth : register(t4);
Texture2D<uint> current_identity : register(t5);
Texture2D<uint> history_identity : register(t6);
float4 previous_at(int2 p){return previous_sample.Load(int3(p,0));}
float depth_at(bool prior,int2 p){return prior?history_depth.Load(int3(p,0)):current_depth.Load(int3(p,0));}
uint4 identity_at(bool prior,int2 p) {
    uint4 v;
    [unroll] for(int i=0;i<4;++i) v[i]=prior?history_identity.Load(int3(p.x*4+i,p.y,0)):current_identity.Load(int3(p.x*4+i,p.y,0));
    return v;
}
#endif
RWTexture2D<float4> resolved_hdr : register(u0);
float3 ycocg(float3 c) {return float3((c.r+2*c.g+c.b)*.25,(c.r-c.b)*.5,(-c.r+2*c.g-c.b)*.25);}
float3 rgb(float3 c) {return float3(c.x+c.y-c.z,c.x+c.z,c.x-c.y-c.z);}
[numthreads(8,8,1)]
void CSMain(uint3 id:SV_DispatchThreadID) {
    if(any(id.xy>=extent_flags.xy)) return;
    int2 p=int2(id.xy);float4 current=current_hdr.Load(int3(p,0));
    // Preserve the authored packed coverage/picking token in alpha exactly.
    resolved_hdr[p]=current;
    if(extent_flags.z==0 || extent_flags.w!=0 || feedback_depth_clip.x==0 || !all(isfinite(current.rgb)))return;
    uint4 identity=identity_at(false,p);
    float4 previous=previous_at(p);float depth=depth_at(false,p);
    if(!any(identity.xy) || !any(identity.zw) || previous.w!=1 || !all(isfinite(previous.xyz)) || previous.z<=0 || !isfinite(depth) || depth<=0)return;
    if(any(previous.xy<0) || any(previous.xy>float2(extent_flags.xy-1)))return;
    int2 first=int2(floor(previous.xy));float2 fraction=frac(previous.xy);float3 history=0;
    [unroll] for(int y=0;y<2;++y) [unroll] for(int x=0;x<2;++x) {
        float weight=(x?fraction.x:1-fraction.x)*(y?fraction.y:1-fraction.y);
        if(weight==0)continue;
        int2 q=first+int2(x,y);
        if(any(identity_at(true,q)!=identity))return;
        float prior_depth=depth_at(true,q);
        if(!isfinite(prior_depth) || prior_depth<=0 || abs(prior_depth-previous.z)>feedback_depth_clip.y+feedback_depth_clip.z*previous.z)return;
        float3 c=history_hdr.Load(int3(q,0)).rgb;if(!all(isfinite(c)))return;
        history+=c*weight;
    }
    float3 low=ycocg(current.rgb),high=low;
    [unroll] for(int y=-1;y<=1;++y) [unroll] for(int x=-1;x<=1;++x) {
        int2 q=p+int2(x,y);
        if(any(q<0) || any(q>=int2(extent_flags.xy)) || any(identity_at(false,q)!=identity))continue;
        float3 c=current_hdr.Load(int3(q,0)).rgb;if(!all(isfinite(c)))continue;
        c=ycocg(c);low=min(low,c);high=max(high,c);
    }
    float3 padding=(high-low)*feedback_depth_clip.w+1e-6;
    float3 clipped=rgb(clamp(ycocg(history),low-padding,high+padding));
    float3 result=lerp(current.rgb,clipped,feedback_depth_clip.x);
    if(all(isfinite(result)))resolved_hdr[p]=float4(max(result,0),current.a);
}
