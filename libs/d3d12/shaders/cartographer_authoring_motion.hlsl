// Same-queue GPU producer. No host per-pixel motion/depth/identity roundtrip.
cbuffer MotionConstants : register(b0) {
    column_major float4x4 inverse_view_projection;
    column_major float4x4 previous_view_projection;
    float4 current_depth_row;
    float4 previous_depth_row;
    uint4 extent_draws; // width,height,palette width,draw count
};
Texture2D<float4> current_hdr : register(t0);
Texture2D<float> device_depth : register(t1);
Texture2D<uint> draw_palette : register(t2);
Texture2D<uint> face_palette : register(t3);
RWTexture2D<uint> geometry : register(u0);
uint draw_word(uint p){return draw_palette.Load(int3(p%extent_draws.z,p/extent_draws.z,0));}
uint face_word(uint p){return face_palette.Load(int3(p%extent_draws.z,p/extent_draws.z,0));}
[numthreads(8,8,1)]
void CSMain(uint3 id:SV_DispatchThreadID) {
    if(any(id.xy>=extent_draws.xy))return;
    uint words[9]={0,0,0,0,0,0,0,0,0};
    float token=current_hdr.Load(int3(id.xy,0)).a;
    // Match the existing exact float24 coverage token. Magenta failures have -1.
    if(isfinite(token) && token>=1 && token<=16777216 && floor(token)==token) {
        uint packed=(uint)token-1,draw=packed/65536,primitive=packed%65536;
        if(draw<extent_draws.w) {
            uint b=draw*24;
            if(primitive<draw_word(b+3)) {
                words[0]=draw_word(b);words[1]=draw_word(b+1);
                uint f=draw_word(b+2)+primitive*2;words[2]=face_word(f);words[3]=face_word(f+1);
                float z=device_depth.Load(int3(id.xy,0));
                float2 ndc=(float2(id.xy)+.5)/float2(extent_draws.xy)*float2(2,-2)+float2(-1,1);
                float4 world=mul(inverse_view_projection,float4(ndc,z,1));
                if(all(isfinite(world)) && abs(world.w)>1e-12 && isfinite(z) && z>=0 && z<=1) {
                    world/=world.w;float current_z=dot(current_depth_row,world);
                    if(isfinite(current_z) && current_z>0)words[8]=asuint(current_z);
                    // Palette matrix columns retain mirrored/nonuniform affine transforms.
                    float4 old_world=0;
                    [unroll]for(uint c=0;c<4;++c) {
                        float4 column=asfloat(uint4(draw_word(b+4+c*4),draw_word(b+5+c*4),draw_word(b+6+c*4),draw_word(b+7+c*4)));
                        old_world+=column*world[c];
                    }
                    float4 clip=mul(previous_view_projection,old_world);
                    float old_z=dot(previous_depth_row,old_world);
                    if(draw_word(b+20)==1 && all(isfinite(clip)) && clip.w>1e-12 && clip.z>=0 && clip.z<=clip.w && isfinite(old_z) && old_z>0 && words[8]!=0) {
                        float2 xy=(clip.xy/clip.w*float2(.5,-.5)+.5)*float2(extent_draws.xy)-.5;
                        words[4]=asuint(xy.x);words[5]=asuint(xy.y);words[6]=asuint(old_z);words[7]=asuint(1.0);
                    }
                }
            }
        }
    }
    [unroll]for(uint slot=0;slot<9;++slot)geometry[id.xy*3+uint2(slot%3,slot/3)]=words[slot];
}
