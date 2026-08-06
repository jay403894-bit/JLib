// Screen-space ambient occlusion.
//
// Why this pass exists: hemisphere ambient varies fill light by which way a surface FACES, which is
// why it barely changed Sponza -- a colonnade is nearly all vertical surfaces, and they all land on
// the same midpoint. The cue an interior actually needs is PROXIMITY: the crease where a column meets
// the floor must be darker than open floor two metres away, and those two points have identical
// normals. Only occlusion can express that.
//
// Method: for each pixel, reconstruct its view-space position from the depth prepass, build a
// hemisphere of sample points around it oriented along the surface normal, and count how many of those
// points end up BEHIND the geometry actually recorded in the depth buffer. Many samples buried behind
// nearby geometry = the point sits in a crevice = occluded.
//
// Two deliberate simplifications, both to avoid extra render targets:
//   - normals come from ddx/ddy of the reconstructed position, not a normal G-buffer. Slightly
//     faceted on curved surfaces, invisible after the blur, and saves a full-screen RT plus a pass.
//   - the sample kernel is a static table rotated per-pixel by hashed noise, not an uploaded kernel
//     plus a noise texture. Same decorrelation, no resources to manage.
// Everything here is WORLD space, deliberately. Working in view space would need the projection
// matrix on its own, but SetCamera takes view and projection already multiplied together -- so view
// space would mean a new API call for every caller to get wrong. The renderer already has viewProj
// and the eye position (the PBR shader needs it), and distance-to-eye substitutes for view-space z
// perfectly well as a depth ordering.
cbuffer SsaoCB : register(b0) {
    row_major float4x4 gViewProj;     // world -> clip, to find where a sample point lands on screen
    row_major float4x4 gInvViewProj;  // clip -> world, to rebuild position from a depth sample
    float3 gEyePos;    float gRadius; // hemisphere radius in WORLD units -- the SCALE of crevice found
    float  gBias;                     // distance slack; stops a flat surface occluding itself
    float  gIntensity;                // scales the occlusion (1 = physical-ish, higher = stronger)
    float  gPad0;
    float  gPad1;
}

Texture2D    gDepth      : register(t0);
SamplerState gPointClamp : register(s0);

struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

// 16 points inside a unit hemisphere about +Z. Lengths deliberately cluster toward the origin
// (the 0.3..1.0 ramp): occlusion falls off with distance, so spending samples near the point being
// shaded resolves contact darkening far better than spreading them evenly through the volume.
static const float3 kKernel[16] = {
    float3( 0.5381f,  0.1856f,  0.4319f), float3( 0.1379f,  0.2486f,  0.4430f),
    float3( 0.3371f,  0.5679f,  0.0057f), float3(-0.6999f, -0.0451f,  0.0019f),
    float3( 0.0689f, -0.1598f,  0.8547f), float3( 0.0560f,  0.0069f,  0.1843f),
    float3(-0.0146f,  0.1402f,  0.0762f), float3( 0.0100f, -0.1924f,  0.0344f),
    float3(-0.3577f, -0.5301f,  0.4358f), float3(-0.3169f,  0.1063f,  0.0158f),
    float3( 0.0103f, -0.5869f,  0.0046f), float3(-0.0897f, -0.4940f,  0.3287f),
    float3( 0.7119f, -0.0154f,  0.0918f), float3(-0.0533f,  0.0596f,  0.5411f),
    float3( 0.0352f, -0.0631f,  0.5460f), float3(-0.4776f,  0.2847f,  0.0271f)
};

// UV + non-linear depth -> world-space position.
float3 WorldPosFromUV(float2 uv, float depth) {
    // UV (y down) back to NDC (y up). Depth is already in NDC's 0..1 range for D3D.
    float4 clip = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, depth, 1.0f);
    float4 w    = mul(clip, gInvViewProj);   // row-vector convention, matching the rest of the renderer
    return w.xyz / w.w;
}

float4 PSMain(VSOut input) : SV_Target {
    float d = gDepth.SampleLevel(gPointClamp, input.uv, 0).r;
    // Depth 1.0 = nothing was drawn here (cleared far plane). Sky must stay fully unoccluded, and
    // reconstructing a position from it would put a point at infinity and poison the whole kernel.
    if (d >= 0.999999f) return 1.0f;

    float3 p = WorldPosFromUV(input.uv, d);
    float3 toEye = gEyePos - p;
    float  pDist = length(toEye);

    // Face normal from screen-space derivatives of position. cross()'s sign depends on which way the
    // rasterizer walks the quad, so rather than assume, force it to face the camera -- a visible
    // surface's normal must point back toward the eye.
    float3 n = normalize(cross(ddx(p), ddy(p)));
    if (dot(n, toEye) < 0.0f) n = -n;

    // Per-pixel rotation, so neighbouring pixels sample different directions. Without this the same
    // 16 offsets everywhere produce structured banding instead of noise, and banding does not blur out.
    float rnd = frac(52.9829189f * frac(dot(input.pos.xy, float2(0.06711056f, 0.00583715f))));
    float ang = rnd * 6.2831853f;
    float3 rvec = float3(cos(ang), sin(ang), 0.0f);
    // Gram-Schmidt against n -> an orthonormal basis whose Z is the surface normal, so the +Z
    // hemisphere above maps onto the surface without any samples landing inside the geometry.
    float3 t = normalize(rvec - n * dot(rvec, n));
    float3 b = cross(n, t);
    float3x3 TBN = float3x3(t, b, n);

    float occlusion = 0.0f;
    [unroll]
    for (int i = 0; i < 16; ++i) {
        float3 samplePos = p + mul(kKernel[i], TBN) * gRadius;

        // Where does that point land on screen, and what depth did the prepass record there?
        float4 off = mul(float4(samplePos, 1.0f), gViewProj);
        if (off.w <= 0.0f) continue;                 // behind the eye -- projecting it is meaningless
        off.xyz /= off.w;
        float2 suv = float2(off.x * 0.5f + 0.5f, 0.5f - off.y * 0.5f);
        if (any(suv < 0.0f) || any(suv > 1.0f)) continue;   // off screen: no depth recorded to compare

        float sd = gDepth.SampleLevel(gPointClamp, suv, 0).r;
        if (sd >= 0.999999f) continue;              // sample fell on sky -- unoccluded
        float3 sPos = WorldPosFromUV(suv, sd);

        float sampleDist = length(gEyePos - samplePos);   // how far out the probe point is
        float sceneDist  = length(gEyePos - sPos);        // how far out the geometry actually is

        // Range check: geometry far in FRONT of this pixel (a foreground column against a distant
        // wall) must not darken it, or every silhouette grows a black halo. Occlusion only counts
        // while the occluder is within about one radius.
        float rangeCheck = smoothstep(0.0f, 1.0f, gRadius / max(1e-4f, abs(pDist - sceneDist)));
        // Recorded geometry NEARER the eye than the probe point means the probe is buried behind it.
        occlusion += (sceneDist <= sampleDist - gBias) ? rangeCheck : 0.0f;
    }

    float ao = saturate(1.0f - (occlusion / 16.0f) * gIntensity);
    return float4(ao, ao, ao, 1.0f);
}
