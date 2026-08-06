// UpdateParticles3D.hlsl -- the phase-1 3D particle simulation (compute). A SELF-RECYCLING fountain: every
// particle integrates under gravity, and when its lifetime runs out it RESPAWNS at the emitter with a fresh
// pseudo-random velocity. That means NO spawn pass, dead-list, or compaction is needed yet -- the fixed pool
// stays full and continuously fountains. (Phase 2 replaces this with the real spawn/dead-list/indirect
// machinery ported from the 2D pools, plus the double-buffer so this dispatch overlaps the graphics queue.)

struct Particle3D {
    float3 position; float size;
    float3 velocity; float lifetime;
    float  age; uint isActive; float2 _pad;
    float4 color;
};
RWStructuredBuffer<Particle3D> ParticleBuffer : register(u0);

// Emitter + sim params (set per pool by RendererCore). gEmitterPos = fountain origin; gSpread = how wide the
// spray cone is; gSpeed = base launch speed; gLifetime = seconds per particle; gColor = spawn color.
cbuffer SimCB : register(b0) {
    float  DeltaTime;
    float  gTime;           // running clock -- seeds the per-particle hash so respawns differ each cycle
    uint   maxParticles;
    float  GravityScale;    // world units/s^2 added to velocity.y each tick (negative = falls)
    float3 gEmitterPos;     float gSpread;
    float  gSpeed;          float gLifetime; float gSize;
    uint   gRespawn;        // 1 = FOUNTAIN (recycle dead particles at the emitter); 0 = BURST pool
                            // (dead particles STAY dead -- SpawnBurst3D.hlsl refills slots on demand)
    float4 gColor;
};

// Cheap hash -> [0,1). Good enough for scattering respawn directions; not for anything that needs quality RNG.
float hash11(uint n) { n = (n ^ 61u) ^ (n >> 16); n *= 9u; n ^= n >> 4; n *= 0x27d4eb2du; n ^= n >> 15; return (n & 0xffffffu) / 16777216.0f; }

// Fresh particle at the emitter: an upward cone, direction jittered by gSpread, speed jittered a little.
Particle3D Spawn(uint i) {
    float s = gTime * 60.0f;   // decorrelate successive respawns of the same slot
    float r1 = hash11(i * 3u + (uint)s + 1u);
    float r2 = hash11(i * 7u + (uint)s + 2u);
    float r3 = hash11(i * 11u + (uint)s + 3u);
    float3 dir = normalize(float3((r1 - 0.5f) * gSpread, 1.0f, (r2 - 0.5f) * gSpread));
    Particle3D p;
    p.position = gEmitterPos;
    p.velocity = dir * (gSpeed * (0.7f + 0.6f * r3));
    p.lifetime = gLifetime * (0.6f + 0.8f * r1);
    p.age      = 0.0f;
    p.isActive = 1u;
    p.size     = gSize;
    p._pad     = float2(0, 0);
    p.color    = gColor;
    return p;
}

[numthreads(256, 1, 1)]
void main(uint3 DTid : SV_DispatchThreadID) {
    uint i = DTid.x;
    if (i >= maxParticles) return;

    Particle3D p = ParticleBuffer[i];
    if (p.isActive == 0u && gRespawn == 0u) return;   // burst pool: dead slots stay dead (ring refills them)
    p.velocity.y += GravityScale * DeltaTime;
    p.position   += p.velocity * DeltaTime;
    p.lifetime   -= DeltaTime;
    p.age        += DeltaTime;
    if (p.lifetime <= 0.0f) {
        if (gRespawn != 0u) p = Spawn(i);   // fountain: recycle in place -- keeps the pool full
        else                p.isActive = 0u; // burst: just die; SpawnBurst3D refills via the ring head
    }
    ParticleBuffer[i] = p;
}
