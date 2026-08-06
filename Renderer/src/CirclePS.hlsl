// Signed-distance circle for Renderer2D's 2D primitives. Same VS_OUTPUT/PS_INPUT layout as
// PixelShader.hlsl -- it pairs with the SAME VertexShader.hlsl (registered together via
// Renderer2D::RegisterEffect); only the pixel stage differs.
//
// WHY A SHADER INSTEAD OF A MESH: a triangle-fan circle is an N-gon, and its flat edges become
// visible as soon as the camera zooms in or the radius grows -- exactly what a 2D editor with zoom
// exposes. This computes coverage from the distance to the quad's center, so the circle is
// mathematically perfect at ANY scale and costs one quad instead of N triangles.
//
// effectParams.x = INNER radius in UV units (0..0.5). 0 draws a filled disc; a positive value
//                  draws a ring/outline between that radius and the quad edge.
// The edge is antialiased with fwidth(), which is a per-pixel screen-space derivative -- so the
// softness stays a constant ~1 pixel no matter how far the view is zoomed in or out.

struct PS_INPUT
{
    float4 pos : SV_POSITION;
    float4 color : COLOR;
    float2 uv : TEXCOORD;
    float useTexture : TEXCOORD1;
    float useAlphaFromRGB : TEXCOORD2;
    float4 effectParams : TEXCOORD3;
};

float4 PSMain(PS_INPUT input) : SV_TARGET
{
    // Distance from the quad's center: 0 at the middle, 0.5 at the edge midpoints.
    float d = length(input.uv - 0.5f);

    // One-pixel-wide feather band, expressed in the same UV units as d.
    float aa = max(fwidth(d), 1e-5f);

    // Outer edge: fully opaque inside, fading to nothing as d crosses 0.5.
    float alpha = 1.0f - smoothstep(0.5f - aa, 0.5f, d);

    // Inner edge (ring mode): carve out everything inside effectParams.x.
    float inner = input.effectParams.x;
    if (inner > 0.0f)
        alpha *= smoothstep(inner - aa, inner, d);

    if (alpha <= 0.0f)
        discard;

    float4 c = input.color;
    c.a *= alpha;
    return c;
}
