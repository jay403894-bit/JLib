// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Joshua Makler. Part of JLib -- see LICENSE at the repository root.

// Screen-space global illumination -- one bounce, by RAY MARCHING the depth buffer.
//
// WHAT THIS REPLACED, AND WHY IT MATTERED. The first version of this pass was SSAO with the answer
// changed: place a sample POINT somewhere in the hemisphere, project it to screen, and if recorded
// geometry sat closer than that point, take its colour. That is an acceptable crude estimator for
// OCCLUSION, where the only question is "is something roughly nearby". It is not an acceptable one
// for light transport, because it never establishes that a ray from this surface actually REACHES
// the surface whose colour it is taking. It gathered from geometry no ray would hit and missed
// geometry a ray would, and at grazing angles the projected sample points smeared along the surface
// in screen space -- which is precisely what produced the horizontal streaking on flat walls.
//
// This version traces. For each ray it steps along the direction, comparing the ray's depth against
// the depth buffer at each step, and takes colour from the FIRST surface it actually intersects.
// That is what makes the result light transport rather than a proximity heuristic.
//
// COSINE-WEIGHTED SAMPLING is the other correctness change. Directions are drawn with density
// proportional to cos(theta), so the Lambert cosine is baked into the sampling rather than applied
// as an ad-hoc weight. The estimator then reduces to the plain MEAN of the radiance found, and the
// pass is properly normalised: irradiance E = PI * mean(L), and a Lambertian surface reflects
// albedo/PI * E = albedo * mean(L). So this shader outputs mean(L), Basic3D_PS multiplies by albedo,
// and `intensity` is a true physical multiplier around 1.0 rather than a free fudge factor.
//
// STILL SCREEN SPACE, and that limit is inherent: a ray that leaves the frustum, or that needs
// geometry hidden behind something nearer the camera, finds nothing. Probes or DDGI are the answer
// to that; this is the cheap approximation that covers most of what is visible.

cbuffer SsgiCB : register(b0) {
    row_major float4x4 gViewProj;     // world -> clip, to project each march step onto the screen
    row_major float4x4 gInvViewProj;  // clip -> world, to rebuild position from a depth sample
    float3 gEyePos;    float gMaxDist;   // how far a ray may travel, WORLD units
    float  gIntensity;                   // 1.0 = one physically-normalised bounce
    float  gMaxLuma;                     // per-sample clamp (firefly suppression)
    float  gThickness;                   // WORLD units; how deep behind the depth buffer still counts
    float  gSteps;                       // march steps per ray
    float  gRays;                        // rays per pixel
    float  gFrame;                       // frame counter, decorrelates the noise between frames
    float2 gPad;
}

Texture2D    gDepth      : register(t0);   // this frame's camera depth prepass (shared with SSAO)
Texture2D    gHistory    : register(t1);   // PREVIOUS frame's linear HDR colour
SamplerState gPointClamp : register(s0);
SamplerState gLinearClamp: register(s1);

struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

float3 WorldPosFromUV(float2 uv, float depth) {
    float4 clip = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, depth, 1.0f);
    float4 w    = mul(clip, gInvViewProj);   // row-vector convention, matching the rest of the renderer
    return w.xyz / w.w;
}

// Cheap hash -> two decorrelated values in [0,1). Seeded with the pixel AND the frame, so the noise
// pattern changes every frame instead of being a fixed screen-space texture that the eye locks onto.
float2 Hash22(float2 p, float f) {
    float3 p3 = frac(float3(p.xyx) * float3(0.1031f, 0.1030f, 0.0973f) + f * 0.0177f);
    p3 += dot(p3, p3.yzx + 33.33f);
    return frac((p3.xx + p3.yz) * p3.zy);
}

float4 PSMain(VSOut input) : SV_Target {
    float d = gDepth.SampleLevel(gPointClamp, input.uv, 0).r;
    // Sky: nothing to receive bounce light, and reconstructing a position here puts it at infinity.
    if (d >= 0.999999f) return float4(0.0f, 0.0f, 0.0f, 1.0f);

    float3 P     = WorldPosFromUV(input.uv, d);
    float3 toEye = gEyePos - P;

    // Face normal from screen-space derivatives, forced to face the camera.
    float3 N = normalize(cross(ddx(P), ddy(P)));
    if (dot(N, toEye) < 0.0f) N = -N;

    // Tangent basis for the hemisphere.
    float3 up = abs(N.y) > 0.99f ? float3(1, 0, 0) : float3(0, 1, 0);
    float3 T  = normalize(cross(up, N));
    float3 B  = cross(N, T);

    const int   steps = (int)gSteps;
    const int   rays  = (int)gRays;
    const float stepLen = gMaxDist / max((float)steps, 1.0f);

    // Start the ray slightly off the surface along N. Without this the first step is inside the
    // surface's own depth and every ray self-intersects immediately -- the light-transport equivalent
    // of shadow acne, and it produces a uniformly black result that looks like the pass doing nothing.
    const float3 origin = P + N * max(stepLen * 0.5f, 0.01f);

    float3 gathered = 0.0f;

    [loop]
    for (int r = 0; r < rays; ++r) {
        float2 rnd = Hash22(input.pos.xy, gFrame + (float)r * 7.13f);

        // COSINE-WEIGHTED direction: r = sqrt(u1) gives density proportional to cos(theta), so the
        // Lambert term is in the sampling and the estimator stays a plain mean.
        float  ra  = sqrt(rnd.x);
        float  phi = 6.2831853f * rnd.y;
        float3 dirT = float3(ra * cos(phi), ra * sin(phi), sqrt(max(0.0f, 1.0f - rnd.x)));
        float3 dir  = normalize(dirT.x * T + dirT.y * B + dirT.z * N);

        // Jitter the start along the ray by up to one step. Without it every ray in every pixel
        // samples the same distances and the misses line up into visible banding rings.
        float  t = stepLen * (0.5f + 0.5f * frac(rnd.x + rnd.y));

        [loop]
        for (int s = 0; s < steps; ++s, t += stepLen) {
            float3 sp = origin + dir * t;

            float4 clip = mul(float4(sp, 1.0f), gViewProj);
            if (clip.w <= 0.0f) break;                       // stepped behind the eye
            float3 ndc = clip.xyz / clip.w;
            float2 suv = float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f);
            if (any(suv < 0.0f) || any(suv > 1.0f)) break;   // left the screen: nothing recorded there

            float sd = gDepth.SampleLevel(gPointClamp, suv, 0).r;
            if (sd >= 0.999999f) break;                      // ray exited into sky

            // Compare along the view direction, using distance-to-eye as the ordering -- the same
            // substitute for view-space z that SSAO uses, and for the same reason: SetCamera takes
            // view and projection already multiplied, so a pure view-space z is not available.
            float3 scenePos  = WorldPosFromUV(suv, sd);
            float  rayDist   = length(gEyePos - sp);
            float  sceneDist = length(gEyePos - scenePos);
            float  delta     = rayDist - sceneDist;          // >0 means the ray is BEHIND the surface

            if (delta > 0.0f) {
                // THICKNESS TEST -- the thing that separates a real intersection from a ray that
                // simply passed behind something. The depth buffer records a surface, not a solid, so
                // without a bound every ray that goes behind any object anywhere reports a hit and
                // the screen fills with false bounce. A hit only counts if the ray is within
                // gThickness of the recorded surface.
                if (delta < gThickness) {
                    float3 c = gHistory.SampleLevel(gLinearClamp, suv, 0).rgb;
                    // Firefly clamp: the history is unbounded HDR, so one emissive texel can dominate
                    // the whole average and then flicker as it moves sub-pixel between frames.
                    float lum = max(c.r, max(c.g, c.b));
                    if (lum > gMaxLuma) c *= gMaxLuma / lum;
                    gathered += c;
                }
                break;   // first hit wins, whether or not it passed the thickness test
            }
        }
    }

    // Cosine-weighted sampling means the estimator is the plain mean. Rays that hit nothing
    // contribute zero, which is correct: no surface was found to supply light.
    gathered *= gIntensity / max((float)rays, 1.0f);
    return float4(max(gathered, 0.0f), 1.0f);
}
