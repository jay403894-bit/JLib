Texture2D particleTex : register(t1);
SamplerState particleSampler : register(s0);

struct VSOutput
{
    float4 pos   : SV_POSITION;
    float4 color : COLOR;
    float2 uv    : TEXCOORD;
};

float4 PSMain(VSOutput input) : SV_TARGET
{
    // Untextured pools bind a 1x1 white texture (see RendererCore::RegisterParticleEffect's
    // fallback), so this Sample() is always valid -- multiplying by white is a no-op, giving a
    // flat-colored quad through the exact same shader as a real textured/animated one.
    float4 texColor = particleTex.Sample(particleSampler, input.uv);
    return float4(input.color.rgb * texColor.rgb, input.color.a * texColor.a);
}
