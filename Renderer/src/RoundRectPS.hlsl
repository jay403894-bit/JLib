// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Joshua Makler. Part of JLib -- see LICENSE at the repository root.

// Signed-distance rounded rectangle for Renderer2D's 2D primitives -- the UI workhorse (panels,
// buttons, pill shapes, capsules). Same VS_OUTPUT/PS_INPUT layout as PixelShader.hlsl, pairing with
// the SAME VertexShader.hlsl via Renderer2D::RegisterEffect; only the pixel stage differs.
//
// WHY PIXEL-SPACE PARAMETERS: the quad's UV is always 0..1 regardless of how the quad is scaled, so
// a corner radius expressed in UV units would be stretched into an ELLIPSE on any non-square rect.
// The half-extents are therefore passed in pixels and the SDF is evaluated in that space, which
// keeps corners perfectly circular at any width/height ratio -- and makes `radius` mean the same
// thing everywhere instead of drifting with the shape.
//
// effectParams.x = half width  in pixels
// effectParams.y = half height in pixels
// effectParams.z = corner radius in pixels (0 = square corners; clamped to the shorter half-extent,
//                  where the shape becomes a capsule/pill)
// effectParams.w = border thickness in pixels (0 = filled; > 0 draws only the outline)

struct PS_INPUT
{
    float4 pos : SV_POSITION;
    float4 color : COLOR;
    float2 uv : TEXCOORD;
    float useTexture : TEXCOORD1;
    float useAlphaFromRGB : TEXCOORD2;
    float4 effectParams : TEXCOORD3;
};

// Standard rounded-box SDF: negative inside, 0 on the boundary, positive outside. `b` is the
// half-extent, `r` the corner radius. Shrinking the box by r and re-expanding via length() is what
// rounds the corners while leaving the straight edges exact.
float sdRoundedBox(float2 p, float2 b, float r)
{
    float2 q = abs(p) - b + r;
    return min(max(q.x, q.y), 0.0f) + length(max(q, 0.0f)) - r;
}

float4 PSMain(PS_INPUT input) : SV_TARGET
{
    float2 half2 = max(input.effectParams.xy, 1e-4f);
    float  radius = input.effectParams.z;
    float  border = input.effectParams.w;

    // A radius past the shorter half-extent is meaningless -- clamp so it saturates into a
    // capsule instead of inverting the distance field.
    radius = clamp(radius, 0.0f, min(half2.x, half2.y));

    // uv 0..1 -> pixel offset from the quad's center.
    float2 p = (input.uv - 0.5f) * (half2 * 2.0f);

    float d = sdRoundedBox(p, half2, radius);

    // fwidth() is a screen-space derivative, so the feather stays ~1 pixel at any camera zoom.
    float aa = max(fwidth(d), 1e-5f);

    // Outer edge: opaque inside (d < 0), fading out across the boundary.
    float alpha = 1.0f - smoothstep(-aa, 0.0f, d);

    // Border mode: subtract the inner shape, leaving a band of `border` pixels along the edge.
    if (border > 0.0f)
    {
        float inner = d + border;                       // inset boundary
        alpha *= smoothstep(-aa, 0.0f, inner);
    }

    if (alpha <= 0.0f)
        discard;

    float4 c = input.color;
    c.a *= alpha;
    return c;
}
