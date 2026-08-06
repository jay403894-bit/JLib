// Same VS_OUTPUT/PS_INPUT layout as PixelShader.hlsl -- pairs with the SAME VertexShader.hlsl
// (registered together via Renderer2D::RegisterEffect), only the pixel stage differs. Draws a
// thin BLACK border near the quad's UV edges and fully transparent everywhere else, so an item
// using this effect can be layered on top of a normally-filled cell (effectID 0) to show just
// its grid line without hiding the fill color underneath -- needs alpha blending enabled
// (Renderer2D::DefaultAlphaBlend()) for the transparent interior to actually show through.

struct PS_INPUT
{
    float4 pos : SV_POSITION;
    float4 color : COLOR;
    float2 uv : TEXCOORD;
    float useTexture : TEXCOORD1;
    float useAlphaFromRGB : TEXCOORD2;
    float4 effectParams : TEXCOORD3; // <-- matches the new VS output
};

// UV distance (0..0.5) from the nearest edge that still counts as "border" -- 0.05 of the
// quad's UV space, i.e. a fixed FRACTION of each cell's size, not a fixed pixel width.
static const float BorderThickness = 0.06f;

float4 PSMain(PS_INPUT input) : SV_TARGET
{
    bool onBorder = input.uv.x < BorderThickness || input.uv.x > (1.0f - BorderThickness) ||
                    input.uv.y < BorderThickness || input.uv.y > (1.0f - BorderThickness);

    if (!onBorder)
        return float4(0.0f, 0.0f, 0.0f, 0.0f); // fully transparent interior -- let the fill show through
    //return float4(0.0f, 0.0f, 0.0f, 1.0f); // opaque black border
    float4 c = input.effectParams;
    if (c.r == 0 && c.g == 0 && c.b == 0 && c.a == 0)
        return float4(0, 0, 0, 1);
    return float4(c.rgb, 1.0f);
}
