// Cartographer depth-only shadow-map shader preparation.

cbuffer ShadowConstants : register(b0)
{
    float4x4 light_view_projection;
    float4x4 model;
};

struct VertexInput
{
    float4 position : POSITION;
};

struct VertexOutput
{
    float4 position : SV_POSITION;
};

VertexOutput VSMain(VertexInput input)
{
    VertexOutput output;
    output.position = mul(light_view_projection, mul(model, input.position));
    return output;
}

float PSMain(VertexOutput input) : SV_DEPTH
{
    return input.position.z / max(input.position.w, 1e-5);
}
