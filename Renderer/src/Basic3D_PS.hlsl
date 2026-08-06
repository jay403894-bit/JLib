// ============================================================================================
// Basic3D_PS.hlsl -- the PIXEL shader. PBR metallic-roughness (Cook-Torrance) with a multi-light
// loop. Runs once per covered pixel; outputs the lit color (SV_TARGET). Shared by BOTH the static
// and skinned pipelines, so it only reads registers BOTH root sigs provide (see Renderer3D.cpp):
//   b0 = camera (ViewProj + camPos) -- we use camPos for the view vector (specular is view-dependent)
//   b3 = lights (ambient + count + array)
//   b4 = material (baseColor/emissive factors + metallic/roughness + a "which maps present" bitmask)
//   t0 = base color, t1 = metallic-roughness (G=rough,B=metal), t2 = emissive, t3 = occlusion,
//   t4 = tangent-space normal map; s0 = sampler
//   Each map is optional (bitmask says which are bound to a REAL texture vs an ignored filler) -- absent
//   maps fall back to the factor alone (or, for the normal map, to the interpolated vertex normal).
// ============================================================================================

// MUST match Basic3D_VS / Skinned3D_VS VSOutput field-for-field. worldPos is NEW (point/spot lights and
// the view vector need the fragment's world position, which clip-space SV_POSITION can't give us).
struct PSInput {
    float4 clip     : SV_POSITION;
    float3 worldPos : POSITION;    // interpolated world-space position of this fragment
    float3 normal   : NORMAL;
    float2 uv       : TEXCOORD;
    float4 tangent  : TANGENT;     // world-space tangent (xyz) + handedness (w) -- builds the TBN for normal mapping
};

// b0: only camPos is used here; ViewProj is declared so camPos lands at the right byte offset (64).
cbuffer Camera : register(b0) { row_major float4x4 ViewProj; float3 gCamPos; float _cpad; }

// b4: per-draw material factors + map bitmask (root constants). Layout MUST match MatConsts in
// Renderer3D's recordBatches. gMapFlags: bit0 = metalRough, bit1 = emissive, bit2 = occlusion, bit3 = normal map.
cbuffer Material : register(b4) {
    float3 gBaseColorFactor; float gMetallic;
    float3 gEmissiveFactor;  float gRoughness;
    uint   gMapFlags;        float3 _mpad;
}

// b3: the light rig. Layout MUST match GpuLight / GpuLights in Renderer3D.h exactly. type: 0=directional,
// 1=point, 2=spot. For directional, `direction` is the light->surface direction and position/range unused.
#define MAX_LIGHTS 16
struct Light {
    float3 position;      float range;         // point/spot world position; range = falloff distance
    float3 direction;     float intensity;     // normalized light direction (directional/spot)
    float3 color;         float type;          // 0=dir, 1=point, 2=spot
    float  spotCosInner;  float spotCosOuter;  float2 _lpad;   // spot cone edges (cosines)
};
cbuffer Lights : register(b3) {
    float3 gAmbient;   uint gLightCount;   // ambient term (applied to albedo) + how many lights are live
    Light  gLights[MAX_LIGHTS];
    // Shadow rig, APPENDED after the array so the existing layout above is untouched. gShadowIndex is
    // which light in gLights casts (-1 = shadows off), and gLightViewProj is that light's camera --
    // the same matrix ShadowDepth_VS rendered the depth map with.
    row_major float4x4 gLightViewProj;
    // gShadowTexel  = 1/shadowMapResolution (PCF tap spacing, in shadow-map UV)
    // gShadowNormalOffset = roughly one shadow texel measured in WORLD units -- see ShadowFactor
    int   gShadowIndex;  float gShadowTexel;  float gShadowNormalOffset;  int gShadowIsPoint;
    // Point-caster only: the cube's origin and its projection range, needed to turn a world-space
    // direction back into the depth value that face recorded.
    float3 gShadowLightPos;  float gShadowFar;
    float  gShadowNear;      float3 _spad;
    // Hemisphere ambient, appended as its own 16-byte row for the same reason the shadow rig was:
    // everything above keeps its offsets. gAmbient doubles as the SKY colour; gHemiMix = 0 reproduces
    // the old flat-constant behaviour bit-for-bit, so scenes that never opt in are untouched.
    float3 gAmbientGround;   float gHemiMix;
    // SSAO: gSsaoTexel = 1/screenSize (the blur tap spacing). gSsaoOn = 0 skips the taps entirely.
    float2 gSsaoTexel;       float gSsaoOn;    float _spad2;
    // IBL: gIblOn = 0 falls back to the hemisphere/flat ambient above. gIblMipCount is the prefiltered
    // chain length, which maps roughness onto a mip level.
    float  gIblOn;           float gIblMipCount;  float2 _spad3;
}

Texture2D    gSsao        : register(t7);   // screen-space occlusion factor (R8), full screen res
// IBL, baked once at load from an HDR environment (see Renderer3D::LoadEnvironment). Adjacent in the
// SRV heap so one descriptor range covers both.
TextureCube  gIrradiance  : register(t8);   // diffuse: cosine-convolved, 32x32/face
TextureCube  gPrefiltered : register(t9);   // specular: one mip per roughness
Texture2D    gAlbedo     : register(t0);   // base color
Texture2D    gMetalRough : register(t1);   // G = roughness, B = metalness  (glTF packing)
Texture2D    gEmissive   : register(t2);   // emissive color
Texture2D    gOcclusion  : register(t3);   // R = ambient occlusion
Texture2D    gNormal     : register(t4);   // tangent-space normal (xyz in [0,1] -> *2-1)
Texture2D    gShadowMap  : register(t5);   // depth from the caster's point of view (see ShadowDepth_VS)
TextureCube  gShadowCube : register(t6);   // six-face depth for a POINT caster, sampled by direction
SamplerState gSampler    : register(s0);
// COMPARISON sampler: SampleCmpLevelZero returns the FILTERED RESULT of "is my depth <= the stored
// depth?" rather than a depth value, so the hardware does the depth test and the bilinear blend in one
// tap. That's what makes a shadow edge soft instead of a staircase of hard texels.
SamplerComparisonState gShadowSampler : register(s1);

// POINT caster: the cube is sampled by DIRECTION, so there are no UVs to offset -- the taps are
// small 3D nudges around the light->surface vector instead. Each face was rendered with a 90-degree
// perspective, so to compare we have to rebuild the projected depth that face would have written.
float ShadowFactorCube(float3 worldPos, float3 N, float3 L) {
    float3 fromLight = worldPos + N * gShadowNormalOffset - gShadowLightPos;

    // Which face a direction lands on is decided by its LARGEST component, and that component is
    // also the view-space Z for that face's frustum. So the projected depth is the standard DX
    // perspective mapping applied to it: z_ndc = f/(f-n) * (1 - n/z).
    float majorAxis = max(abs(fromLight.x), max(abs(fromLight.y), abs(fromLight.z)));
    if (majorAxis < 1e-4f) return 1.0f;
    float f = gShadowFar, n = gShadowNear;
    float depth = (f / (f - n)) * (1.0f - n / majorAxis);
    depth -= 0.0015f;   // small constant slack; the normal offset above does the real work

    // Six taps around the direction. Offsets are in WORLD units scaled to roughly a texel, which is
    // the cube-map equivalent of stepping one texel in UV.
    static const float3 kOff[6] = {
        float3( 1, 0, 0), float3(-1, 0, 0), float3( 0, 1, 0),
        float3( 0,-1, 0), float3( 0, 0, 1), float3( 0, 0,-1)
    };
    float r = gShadowNormalOffset * 1.5f;
    float sum = gShadowCube.SampleCmpLevelZero(gShadowSampler, fromLight, depth);
    [unroll]
    for (int i = 0; i < 6; ++i)
        sum += gShadowCube.SampleCmpLevelZero(gShadowSampler, fromLight + kOff[i] * r, depth);
    return sum / 7.0f;
}

// Returns how lit this world position is: 1 = fully lit, 0 = fully shadowed.
// N and L are the surface normal and the direction TO the light, used for the slope-scaled bias.
float ShadowFactor(float3 worldPos, float3 N, float3 L) {
    if (gShadowIsPoint != 0) return ShadowFactorCube(worldPos, N, L);
    if (gShadowIndex < 0) return 1.0f;                       // shadows disabled

    // NORMAL OFFSET, not depth bias. Depth bias fakes how FAR a surface is from the light, which
    // detaches shadows from their casters ("peter-panning" -- the shadow floats a pixel off the
    // object's base). Offsetting the LOOKUP POSITION along the surface normal instead moves the
    // sample out of its own self-shadowing zone without lying about depth, so contact stays tight.
    // Scaled by (1 - NdotL): surfaces facing the light barely need it, grazing ones need the most,
    // which is where acne would otherwise appear.
    // The 1.6 scale compensates for dropping front-face culling: the depth pass now records each
    // caster's NEAR surface, so a surface sits closer to its own recorded depth and needs a slightly
    // larger push out of the self-shadowing zone. Still far below the point where contact detaches.
    float ndl0 = saturate(dot(N, L));
    float3 samplePos = worldPos + N * gShadowNormalOffset * (1.6f * (1.0f - ndl0) + 0.4f);

    float4 lp = mul(float4(samplePos, 1.0f), gLightViewProj);
    // A SPOT caster is a perspective projection, so w carries real depth and can go <= 0 for anything
    // behind the light. Dividing by that mirrors the coordinates and would shadow geometry behind the
    // lamp. (Directional casters are orthographic and always have w == 1, so this costs them nothing.)
    if (lp.w <= 0.0f) return 1.0f;
    lp.xyz /= lp.w;                                          // -> light clip space
    // Outside the shadow map's frustum there is no information; treat it as lit rather than guessing,
    // so geometry beyond the shadow bounds doesn't turn black.
    if (lp.x < -1.0f || lp.x > 1.0f || lp.y < -1.0f || lp.y > 1.0f || lp.z > 1.0f) return 1.0f;

    // Clip space (-1..1, Y up) -> texture space (0..1, Y down).
    float2 uv = float2(lp.x * 0.5f + 0.5f, -lp.y * 0.5f + 0.5f);

    // A SMALL residual depth bias on top of the normal offset above -- just enough to absorb depth
    // quantization, not enough to visibly detach anything. The normal offset does the real work;
    // this was 0.0015 and that (plus the rasterizer's own bias) is what caused the visible gap.
    // Residual depth bias. A PERSPECTIVE caster (spot) needs more than an orthographic one: its depth
    // values are non-linear, so a given world-space error maps to a much smaller depth delta far from
    // the light. Raising the near plane on the CPU side does most of the work; this covers the rest.
    float ndl   = saturate(dot(N, L));
    float scale = (gShadowIsPoint != 0 || lp.w > 1.0001f) ? 3.0f : 1.0f;   // w != 1 => perspective
    float bias  = max(0.0003f * scale * (1.0f - ndl), 0.00005f * scale);
    float depth = lp.z - bias;

    // 3x3 PCF: nine comparison taps, each already bilinearly filtered by the hardware. Cheap, and
    // enough to turn a hard aliased edge into a soft one at this resolution.
    float sum = 0.0f;
    [unroll]
    for (int y = -1; y <= 1; ++y)
        [unroll]
        for (int x = -1; x <= 1; ++x)
            sum += gShadowMap.SampleCmpLevelZero(gShadowSampler,
                                                 uv + float2(x, y) * gShadowTexel, depth);
    return sum / 9.0f;
}

static const float PI = 3.14159265f;

// --- Cook-Torrance terms ---
// GGX / Trowbridge-Reitz normal distribution: how much of the microsurface is aligned with the half vector.
float D_GGX(float NdotH, float rough) {
    float a  = rough * rough;
    float a2 = a * a;
    float d  = (NdotH * NdotH) * (a2 - 1.0f) + 1.0f;
    return a2 / max(PI * d * d, 1e-7f);
}
// Smith geometry (Schlick-GGX), split for the direct-light k. Self-shadowing/masking of microfacets.
float G_SchlickGGX(float NdotX, float k) { return NdotX / (NdotX * (1.0f - k) + k); }
float G_Smith(float NdotV, float NdotL, float rough) {
    float k = (rough + 1.0f);
    k = (k * k) / 8.0f;                      // direct-lighting remap
    return G_SchlickGGX(NdotV, k) * G_SchlickGGX(NdotL, k);
}
// Fresnel-Schlick: reflectance grows toward grazing angles.
float3 F_Schlick(float cosTheta, float3 F0) {
    return F0 + (1.0f - F0) * pow(saturate(1.0f - cosTheta), 5.0f);
}

// Analytic fit to the split-sum BRDF integration term (Karis, "Mobile HDR"). The textbook approach
// bakes this into a 2D lookup texture; this polynomial reproduces it closely enough to be
// indistinguishable in practice, and costs one less texture, one less bake pass and one less SRV slot.
// Returns (scale, bias) to apply to F0.
float2 EnvBRDFApprox(float roughness, float NoV) {
    const float4 c0 = float4(-1.0f, -0.0275f, -0.572f,  0.022f);
    const float4 c1 = float4( 1.0f,  0.0425f,  1.040f, -0.040f);
    float4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28f * NoV)) * r.x + r.y;
    return float2(-1.04f, 1.04f) * a004 + r.zw;
}

float4 PSMain(PSInput input) : SV_TARGET {
    float3 N = normalize(input.normal);           // interpolation shortens it -- renormalize
    // Normal mapping: perturb N by the tangent-space normal map BEFORE any lighting uses it. Gated on the
    // per-draw uniform bit3, so untextured meshes (zero tangent) skip it entirely. Build an orthonormal
    // TBN: T from the interpolated tangent (Gram-Schmidt against N to undo interpolation skew), B from
    // cross(N,T) with the glTF handedness sign. nm is the sampled normal remapped from [0,1] to [-1,1].
    if (gMapFlags & 8u) {
        float3 nm = gNormal.Sample(gSampler, input.uv).xyz * 2.0f - 1.0f;  // sample under the UNIFORM branch (mip-safe)
        float3 Traw = input.tangent.xyz - N * dot(N, input.tangent.xyz);   // Gram-Schmidt orthogonalize
        float  tl   = dot(Traw, Traw);
        if (tl > 1e-10f) {                        // valid tangent -> perturb; degenerate -> keep the vertex normal
            float3 T = Traw * rsqrt(tl);
            float3 B = cross(N, T) * input.tangent.w;
            N = normalize(nm.x * T + nm.y * B + nm.z * N);
        }
    }
    float3 V = normalize(gCamPos - input.worldPos); // surface -> camera
    float  NdotV = saturate(dot(N, V));

    // Base color = factor x texture. metallic/roughness = factor, optionally modulated by the MR map
    // (glTF packs roughness in G, metalness in B). gMapFlags gates the optional maps (bit0 = MR present);
    // the branch is on a per-draw constant (uniform), so sampling inside it is derivative-safe.
    float3 albedo   = gBaseColorFactor.rgb * gAlbedo.Sample(gSampler, input.uv).rgb;
    float  metallic = gMetallic;
    float  rough    = gRoughness;
    if (gMapFlags & 1u) {
        float3 mr = gMetalRough.Sample(gSampler, input.uv).rgb;
        rough    = gRoughness * mr.g;
        metallic = gMetallic  * mr.b;
    }
    metallic = saturate(metallic);
    rough    = clamp(rough, 0.045f, 1.0f);   // floor avoids a NaN-prone perfect mirror

    // Base reflectance: dielectrics reflect a flat ~4%; metals tint their reflection with the albedo.
    float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);

    float3 Lo = float3(0.0f, 0.0f, 0.0f);   // accumulated outgoing radiance from all direct lights
    uint count = min(gLightCount, (uint)MAX_LIGHTS);
    for (uint i = 0; i < count; ++i) {
        Light lt = gLights[i];

        // Light direction (surface->light) + radiance, per type.
        float3 L;
        float  atten = 1.0f;
        if (lt.type < 0.5f) {
            L = normalize(-lt.direction);          // directional: parallel rays, no attenuation
        } else {
            float3 toLight = lt.position - input.worldPos;
            float  dist    = length(toLight);
            L = toLight / max(dist, 1e-4f);
            // Inverse-square with a smooth range cutoff (keeps lights local instead of infinite).
            float r  = max(lt.range, 1e-3f);
            float f  = saturate(1.0f - pow(dist / r, 4.0f));
            atten    = (f * f) / (dist * dist + 1.0f);
            if (lt.type > 1.5f) {                  // spot: cone falloff on top of point attenuation
                float cd = dot(normalize(-lt.direction), L);
                atten *= smoothstep(lt.spotCosOuter, lt.spotCosInner, cd);
            }
        }

        float3 radiance = lt.color * lt.intensity * atten;
        float3 H = normalize(V + L);
        float NdotL = saturate(dot(N, L));
        float NdotH = saturate(dot(N, H));
        float VdotH = saturate(dot(V, H));

        float  D = D_GGX(NdotH, rough);
        float  G = G_Smith(NdotV, NdotL, rough);
        float3 F = F_Schlick(VdotH, F0);

        float3 spec = (D * G * F) / max(4.0f * NdotV * NdotL, 1e-4f);
        // Energy conservation: what isn't specularly reflected is diffusely scattered -- and metals have
        // no diffuse. kD is the leftover diffuse fraction.
        float3 kD = (1.0f - F) * (1.0f - metallic);

        // Only the designated caster is shadowed -- one shadow map, one light. Everything else stays
        // unshadowed rather than reusing a map that wasn't rendered from its point of view.
        float shadow = ((int)i == gShadowIndex) ? ShadowFactor(input.worldPos, N, L) : 1.0f;

        Lo += (kD * albedo / PI + spec) * radiance * NdotL * shadow;
    }

    // Constant ambient stands in for environment lighting. Split it the same way direct light is split:
    // a diffuse part (killed for metals via kDamb) PLUS a specular part, so METAL surfaces -- which have
    // zero diffuse -- still catch ambient as a base reflection instead of going black everywhere a direct
    // highlight isn't. Fresnel at the view angle stands in for the environment's reflectance; it also gives
    // a bright rim at grazing angles, which reads as "metal".
    float3 Famb    = F_Schlick(NdotV, F0);
    float3 kDamb   = (1.0f - Famb) * (1.0f - metallic);
    // Occlusion (R of the AO map) darkens AMBIENT/indirect only -- direct lights are never occluded by it.
    float ao = 1.0f;
    if (gMapFlags & 4u) ao = gOcclusion.Sample(gSampler, input.uv).r;
    // SSAO folds into the SAME term as the occlusion map -- both answer "how much indirect light
    // reaches this point", one authored per-material, one computed per-frame from the depth buffer.
    // Multiplying them means a model with a baked AO map still gets contact darkening from the scene
    // around it. Screen UV comes from SV_Position, so nothing extra has to be interpolated.
    if (gSsaoOn > 0.5f) {
        float2 suv = input.clip.xy * gSsaoTexel;
        // 3x3 box blur done HERE rather than as its own fullscreen pass. SSAO's per-pixel kernel
        // rotation trades banding for noise, and that noise has to be smoothed; taking 9 taps of an
        // R8 texture inline costs less than another render target, PSO and pass would.
        float sum = 0.0f;
        [unroll]
        for (int oy = -1; oy <= 1; ++oy)
            [unroll]
            for (int ox = -1; ox <= 1; ++ox)
                sum += gSsao.Sample(gSampler, suv + float2(ox, oy) * gSsaoTexel).r;
        ao *= sum / 9.0f;
    }
    // Hemisphere split: sky colour on upward-facing surfaces, ground-bounce colour on downward-facing
    // ones, blended by the normal's up-ness. One constant ambient lights a floor, a wall and an arch
    // soffit IDENTICALLY, which is most of what makes an unlit interior read as flat -- this costs a
    // single lerp and restores the orientation cue that real indirect light carries. It is not GI: it
    // knows nothing about what is actually above or below a surface, only which way the surface faces.
    float3 ambient;
    if (gIblOn > 0.5f) {
        // IMAGE-BASED LIGHTING. The constant/hemisphere terms below are stand-ins for exactly this:
        // ambient that varies with direction because it came from a real environment. Diffuse reads
        // the cosine-convolved irradiance along N; specular reads the prefiltered chain along the
        // reflection vector, with roughness selecting the mip -- which is why a rough metal blurs its
        // reflection and a smooth one mirrors, from one sample each.
        float3 irradiance = gIrradiance.Sample(gSampler, N).rgb;
        float3 R          = reflect(-V, N);
        float3 prefiltered = gPrefiltered.SampleLevel(gSampler, R,
                                                      rough * max(gIblMipCount - 1.0f, 0.0f)).rgb;
        float2 ab   = EnvBRDFApprox(rough, NdotV);
        float3 spec = prefiltered * (F0 * ab.x + ab.y);
        ambient = (kDamb * albedo * irradiance + spec) * ao;
    } else {
        float3 hemiAmbient  = lerp(gAmbientGround, gAmbient, saturate(N.y * 0.5f + 0.5f));
        float3 ambientColor = lerp(gAmbient, hemiAmbient, gHemiMix);
        ambient = ambientColor * (kDamb * albedo + Famb) * ao;
    }

    // Emissive = factor x texture, added after lighting (self-illuminated surfaces -- the glowing bits).
    float3 emissive = gEmissiveFactor;
    if (gMapFlags & 2u) emissive *= gEmissive.Sample(gSampler, input.uv).rgb;

    float3 color = ambient + Lo + emissive;

    // Reinhard tone map so multiple/bright lights roll off to white instead of hard-clipping. (Proper
    // HDR + gamma is the Environment milestone; this keeps the output bounded and reasonable for now.)
    color = color / (color + 1.0f);
    return float4(color, 1.0f);
}
