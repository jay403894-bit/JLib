// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Joshua Makler. Part of JLib -- see LICENSE at the repository root.

// Keep your GlobalUniforms as they are
cbuffer GlobalUniforms : register(b0)
{
    float time;
    float2 cameraPos;   // world-space point the camera is centered on (unused by sprites --
                         // Renderer2D::Submit already reprojects through Camera2D on the CPU
                         // before this shader ever runs; kept here only for layout parity with
                         // ParticleVS.hlsl, which shares this exact cbuffer).
    float padding1;
    float2 screenSize;
    float aspectRatio;
    float cameraZoom;
};

// Replace cbuffer ObjectData with a StructuredBuffer
struct ObjectData
{
    float2 pos;
    float2 size;
    float4 color;
    float useTexture;
    float rotation;
    float useAlphaFromRGB; // 1 = derive alpha from texture RGB luminance (repurposed padding float)
    // Horizontal shear (was the spare alignment float). Makes the instance transform a full 2D
    // AFFINE map instead of scale+rotate+translate, which is what an arbitrary triangle needs.
    // 0 for every ordinary sprite, so the math below is unchanged for them.
    float shear;
    float2 uvOffset; // Per-instance UV sub-rect: final uv = mesh.uv * uvScale + uvOffset
    float2 uvScale;
    // Free-form per-effect payload (see Renderer.h ObjectData::effectParams). This DEFAULT
    // shader ignores it -- present only so the StructuredBuffer stride matches the C++ side.
    // A different effect's own .hlsl files can read g_ObjectBuffer[instanceID].effectParams
    // however they like without touching this file.
    float4 effectParams;
};

StructuredBuffer<ObjectData> g_ObjectBuffer : register(t1);

struct VS_INPUT
{
    float3 pos : POSITION;
    float2 uv : TEXCOORD;
};

struct VS_OUTPUT
{
    float4 pos : SV_POSITION;
    float4 color : COLOR;
    float2 uv : TEXCOORD;
    float useTexture : TEXCOORD1;
    float useAlphaFromRGB : TEXCOORD2;
    float4 effectParams : TEXCOORD3; // MUST be here or 
};

// Add SV_InstanceID to the function parameters
VS_OUTPUT VSMain(VS_INPUT input, uint instanceID : SV_InstanceID)
{
    VS_OUTPUT output;

    // Access the data for the current instance
    ObjectData obj = g_ObjectBuffer[instanceID];

    // 1. Scale (+ shear). With shear == 0 this is exactly `input.pos.xy * obj.size` as before --
    // the upper-triangular matrix [[size.x, shear], [0, size.y]]. Combined with the rotation
    // below it forms a general 2x2, so any triangle is reachable from one reference mesh.
    float2 scaled = float2(input.pos.x * obj.size.x + input.pos.y * obj.shear,
                           input.pos.y * obj.size.y);

    // 2. Aspect Ratio Correction
    scaled.x *= aspectRatio;

    // 3. Rotate
    float rad = obj.rotation * (3.14159f / 180.0f);
    float s, c;
    sincos(rad, s, c);
    
    float2 rotated;
    rotated.x = scaled.x * c - scaled.y * s;
    rotated.y = scaled.x * s + scaled.y * c;

    // 4. Un-correct Aspect Ratio
    rotated.x /= aspectRatio;

    // 5. Translate
    output.pos = float4(rotated + obj.pos, 0.0f, 1.0f);
    output.color = obj.color;
    output.uv = input.uv * obj.uvScale + obj.uvOffset;
    output.useTexture = obj.useTexture;
    output.useAlphaFromRGB = obj.useAlphaFromRGB;
    output.effectParams = obj.effectParams;
    return output;
}