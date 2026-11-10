// Independent bounded authoring-material interpreter, packet ABI v1.
// Exact sampler intent is evaluated per map. Typed sRGB Load decodes texels
// before manual filtering; data/normal resources use linear UNORM views.
Texture2D<float4> base_color_texture : register(t0);
Texture2D<float4> metallic_roughness_texture : register(t1);
Texture2D<float4> normal_texture : register(t2);
Texture2D<float4> occlusion_texture : register(t3);
Texture2D<float4> emissive_texture : register(t4);
Texture2D<float4> material_program : register(t5);

float4 program_word(uint word) { return material_program.Load(int3(word % 32, word / 32, 0)); }
int address_texel(int x, int size, uint mode) {
    if (mode == 2) return clamp(x, 0, size - 1);
    int period = mode == 0 ? size : size * 2;
    x = (x % period + period) % period;
    return x < size ? x : period - 1 - x;
}
float4 load_map(uint slot, int2 p, uint mip) {
    if (slot == 0) return base_color_texture.Load(int3(p, mip));
    if (slot == 1) return metallic_roughness_texture.Load(int3(p, mip));
    if (slot == 2) return normal_texture.Load(int3(p, mip));
    if (slot == 3) return occlusion_texture.Load(int3(p, mip));
    return emissive_texture.Load(int3(p, mip));
}
float4 addressed_map(uint slot, int2 p, uint mip, uint2 size, uint flags) {
    return load_map(slot, int2(address_texel(p.x, size.x, (flags >> 3) & 3),
                              address_texel(p.y, size.y, (flags >> 5) & 3)), mip);
}
float4 sample_level(uint slot, float2 uv, uint mip, bool minifying, float4 metadata) {
    uint2 size = max(uint2(1,1), uint2(metadata.xy) >> mip);
    uint flags = uint(metadata.w);
    float2 texel = uv * size - .5;
    if ((flags & (minifying ? 1 : 2)) == 0)
        return addressed_map(slot, int2(floor(texel + .5)), mip, size, flags);
    int2 p = int2(floor(texel));float2 t = frac(texel);
    return lerp(lerp(addressed_map(slot,p,mip,size,flags), addressed_map(slot,p+int2(1,0),mip,size,flags),t.x),
                lerp(addressed_map(slot,p+int2(0,1),mip,size,flags), addressed_map(slot,p+int2(1,1),mip,size,flags),t.x),t.y);
}
float2 rotate_uv(float2 value, float2 cs) {
    return float2(value.x*cs.x-value.y*cs.y,value.x*cs.y+value.y*cs.x);
}
float4 sample_map(uint slot, float2 uv, float2 dx, float2 dy, inout bool valid) {
    float4 metadata=program_word(4+slot*4);
    float4 transform=program_word(5+slot*4), rotation=program_word(6+slot*4);
    uv=rotate_uv((uv-rotation.zw)*transform.zw,rotation.xy)+rotation.zw+transform.xy;
    dx=rotate_uv(dx*transform.zw,rotation.xy);dy=rotate_uv(dy*transform.zw,rotation.xy);
    float rho=max(length(dx*metadata.xy),length(dy*metadata.xy));
    bool finite=all(isfinite(uv)) && all(isfinite(dx)) && all(isfinite(dy)) && isfinite(rho) && all(abs(uv)<=1e6);
    valid=valid && finite;
    if(!finite) return 0;
    float lod=clamp(log2(max(1.0,rho)),0.0,metadata.z-1);
    uint lo=uint(floor(lod));uint flags=uint(metadata.w);
    float4 value;
    if((flags&4)==0) value=sample_level(slot,uv,uint(floor(lod+.5)),rho>1,metadata);
    else value=lerp(sample_level(slot,uv,lo,rho>1,metadata),sample_level(slot,uv,min(lo+1,uint(metadata.z)-1),rho>1,metadata),frac(lod));
    uint4 mapping=uint4(program_word(7+slot*4));float4 mapped;
    [unroll] for(uint c=0;c<4;++c) {
        uint channel=mapping[c];
        if(channel<4) mapped[c]=value[channel];
        else if(channel<8) mapped[c]=1-value[channel-4];
        else mapped[c]=channel==8 ? 0 : 1;
    }
    return mapped;
}
struct EvaluatedMaterial {
    float4 base;
    float3 emission, tangent_normal;
    float roughness, metallic, coat, coat_roughness, ao;
    bool normal_mapped, valid;
};
EvaluatedMaterial evaluate_material(float2 uv,float4 base,float3 emission,float4 factors,float f0) {
    EvaluatedMaterial m;
    m.base=base;m.emission=emission;m.metallic=factors.x;m.roughness=factors.y;
    m.coat=program_word(3).x;m.coat_roughness=program_word(3).y;m.ao=1;
    m.tangent_normal=float3(0,0,1);m.normal_mapped=false;m.valid=true;
    float2 ux=ddx(uv),uy=ddy(uv);
    float4 r[32],rx[32],ry[32];
    float4 header=program_word(0);
    [loop] for(uint i=0;i<uint(header.y);++i) {
        uint4 instruction=uint4(program_word(24+i*3)), slots=uint4(program_word(25+i*3));
        float4 value=0,dx=0,dy=0;
        uint op=instruction.x,parameter=instruction.z;
        if(op<2) value=program_word(26+i*3);
        else if(op==2 || op==3) {
            if(parameter==0) value=base;
            else if(parameter==1) value=float4(emission,1);
            else if(parameter==2) value.x=base.a;
            else if(parameter==3) value.x=factors.y;
            else if(parameter==4) value.x=factors.x;
            else if(parameter==5) value.x=f0;
            else if(parameter==6) value.x=m.coat;
            else if(parameter==7) value.x=m.coat_roughness;
            else if(parameter==8) {value.x=uv.x;dx.x=ux.x;dy.x=uy.x;}
            else {value.x=uv.y;dx.x=ux.y;dy.x=uy.y;}
        } else if(op==4) {
            float2 coordinate=float2(r[slots.y].x,r[slots.z].x);
            float2 cx=float2(rx[slots.y].x,rx[slots.z].x),cy=float2(ry[slots.y].x,ry[slots.z].x);
            value=sample_map(instruction.w,coordinate,cx,cy,m.valid);
            dx=sample_map(instruction.w,coordinate+cx,cx,cy,m.valid)-value;
            dy=sample_map(instruction.w,coordinate+cy,cx,cy,m.valid)-value;
        } else {
            float4 a=r[slots.y],b=r[slots.z],ax=rx[slots.y],ay=ry[slots.y],bx=rx[slots.z],by=ry[slots.z];
            if(op==6) {value=a+b;dx=ax+bx;dy=ay+by;}
            else if(op==7) {value=a*b;dx=ax*b+a*bx;dy=ay*b+a*by;}
            else {
                float w=r[slots.w].x;
                value=lerp(a,b,w);dx=lerp(ax,bx,w)+(b-a)*rx[slots.w].x;dy=lerp(ay,by,w)+(b-a)*ry[slots.w].x;
            }
            if(instruction.y==0) {value.yzw=0;dx.yzw=0;dy.yzw=0;}
        }
        m.valid=m.valid && all(isfinite(value)) && all(isfinite(dx)) && all(isfinite(dy)) && all(abs(value)<=1e12);
        r[slots.x]=value;rx[slots.x]=dx;ry[slots.x]=dy;
    }
    if(header.y>0) {
        int4 outputs=int4(program_word(1)), optional_outputs=int4(program_word(2));
        m.base=float4(r[outputs.x].rgb,r[outputs.y].x);m.roughness=r[outputs.z].x;m.metallic=r[outputs.w].x;
        if(optional_outputs.x>=0) m.emission=r[optional_outputs.x].rgb;
        if(optional_outputs.y>=0) m.coat=r[optional_outputs.y].x;
        if(optional_outputs.z>=0) m.coat_roughness=r[optional_outputs.z].x;
    }
    if(program_word(4).x>0) {
        float4 t=sample_map(0,uv,ux,uy,m.valid);
        if((uint(header.z)&1)==0) m.base.rgb*=t.rgb;
        m.base.a*=t.a;
    }
    if(program_word(8).x>0) {float4 t=sample_map(1,uv,ux,uy,m.valid);m.roughness*=t.g;m.metallic*=t.b;}
    if(program_word(12).x>0) {
        float3 n=sample_map(2,uv,ux,uy,m.valid).xyz*2-1;
        n.xy*=factors.z;n.y*=((uint(program_word(12).w)&128)!=0) ? 1 : -1;
        m.valid=m.valid && dot(n,n)>1e-12;m.tangent_normal=normalize(n);m.normal_mapped=true;
    }
    // AO modulates diffuse environment only; it is never a GI surrogate or
    // a substitute for spatial directional shadows.
    if(program_word(16).x>0) {
        float occlusion=sample_map(3,uv,ux,uy,m.valid).r;
        m.ao=1+factors.w*(occlusion-1);
        m.valid=m.valid && isfinite(occlusion);
    }
    if(program_word(20).x>0 && (uint(header.z)&2)==0) m.emission*=sample_map(4,uv,ux,uy,m.valid).rgb;
    m.valid=m.valid && all(isfinite(m.base)) && all(m.base>=0) && m.base.a<=1
        && all(isfinite(m.emission)) && all(m.emission>=0)
        && isfinite(m.roughness) && m.roughness>=0 && m.roughness<=1
        && isfinite(m.metallic) && m.metallic>=0 && m.metallic<=1
        && isfinite(m.coat) && m.coat>=0 && m.coat<=1
        && isfinite(m.coat_roughness) && m.coat_roughness>=0 && m.coat_roughness<=1;
    return m;
}
