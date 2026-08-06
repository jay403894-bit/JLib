// Update your input structure to include texture coordinates (UVs)
// Add a constant buffer to your shader
struct PS_INPUT
{
    float4 pos : SV_POSITION;
    float4 color : COLOR;
    float2 uv : TEXCOORD; // Add this
    float useTexture : TEXCOORD1; // Add this
    float useAlphaFromRGB : TEXCOORD2;
    float4 effectParams : TEXCOORD3; // unused here -- just matches the shared VS output signature
};

// Add these at the top of your shader file
Texture2D g_texture : register(t0);
SamplerState g_sampler : register(s0);

float4 PSMain(PS_INPUT input) : SV_TARGET
{
    float4 finalColor = input.color;

    // Only sample if the texture flag is set
    if (input.useTexture > 0)
    {
        float4 tex = g_texture.Sample(g_sampler, input.uv);
        if (input.useAlphaFromRGB > 0)
        {
            // BMFont atlas with no usable alpha channel (outline=0 -> alpha is meaningless):
            // glyph ink is baked into RGB as white-on-black instead of real transparency.
            // Derive coverage from RGB luminance and use it as alpha; keep the REQUESTED
            // color (input.color) for RGB instead of the texture's, so text tints correctly
            // and empty (black-in-source) texels become transparent instead of solid black.
            float coverage = max(tex.r, max(tex.g, tex.b));
            finalColor.a *= coverage;
        }
        else
        {
            finalColor *= tex;
        }
    }

    return finalColor;
}