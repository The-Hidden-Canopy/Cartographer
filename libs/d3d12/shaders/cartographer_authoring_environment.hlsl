// Fixed linear-HDR atlas. Rect-local filtering prevents mip/LUT row bleed;
// longitude wraps, latitude clamps. Native hardware mips are not substituted
// for directional roughness convolution.
Texture2D<float4> authoring_environment : register(t8);
float2 environment_uv(float3 d) {
    d=normalize(d);
    float longitude=abs(d.x)+abs(d.z)<1e-12 ? 0 : atan2(d.z,d.x);
    return float2(longitude/(2*3.141592653589793)+.5,acos(clamp(d.y,-1,1))/3.141592653589793);
}
float4 environment_rect(float2 uv,int row,int width,int height,bool wrap,int column=0) {
    float2 pixel=uv*float2(width,height)-.5;
    int2 first=int2(floor(pixel));float2 fraction=frac(pixel);float4 result=0;
    [unroll] for(int y=0;y<2;++y) [unroll] for(int x=0;x<2;++x) {
        int sx=wrap?((first.x+x)%width+width)%width:clamp(first.x+x,0,width-1);
        int sy=clamp(first.y+y,0,height-1);
        result+=authoring_environment.Load(int3(column+sx,row+sy,0))*(x?fraction.x:1-fraction.x)*(y?fraction.y:1-fraction.y);
    }
    return result;
}
float3 environment_specular(float3 direction,float roughness) {
    float level=saturate(roughness)*6;int a=int(floor(level)),b=min(a+1,6);
    int wa=128>>min(a,4),wb=128>>min(b,4);float2 uv=environment_uv(direction);
    int ra=a<5?128-wa:120,rb=b<5?128-wb:120;
    int ca=a<5?0:(a-4)*8,cb=b<5?0:(b-4)*8;
    return lerp(environment_rect(uv,ra,wa,wa/2,true,ca).rgb,
                environment_rect(uv,rb,wb,wb/2,true,cb).rgb,level-a);
}
float3 environment_evaluate(float3 n,float3 v,float3 base,float3 f0,float roughness,
    float metallic,float coat,float coat_roughness,float ao,float4 settings) {
    float nv=saturate(dot(n,v));
    if(settings.w==0 || settings.z==0 || nv<=0) return 0;
    float3 reflected=2*nv*n-v;
    n=float3(settings.x*n.x-settings.y*n.z,n.y,settings.y*n.x+settings.x*n.z);
    reflected=float3(settings.x*reflected.x-settings.y*reflected.z,reflected.y,
        settings.y*reflected.x+settings.x*reflected.z);
    float3 diffuse=environment_rect(environment_uv(n),127,32,16,true).rgb;
    float2 brdf=environment_rect(float2(nv,roughness),143,64,64,false).rg;
    float2 coat_brdf=environment_rect(float2(nv,coat_roughness),143,64,64,false).rg;
    float3 specular=environment_specular(reflected,roughness)*(f0*brdf.x+brdf.y);
    diffuse*=base*(1-f0)*(1-metallic)*ao;
    float attenuation=1-coat*(.04+.96*pow(1-nv,5));
    float3 coat_specular=environment_specular(reflected,coat_roughness)*coat*(.04*coat_brdf.x+coat_brdf.y);
    return ((diffuse+specular)*attenuation+coat_specular)*settings.z;
}
