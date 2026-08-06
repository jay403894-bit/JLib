// SpawnBurst3D.hlsl -- writes ONE burst of particles into a ring-buffer burst pool (phase 2A).
// The CPU owns the ring head (single writer), so this shader gets an explicit slot range: thread i
// writes slot (gStartSlot + i) % gPoolSize. No dead-list/counters -- transient burst particles are
// simply overwritten when the ring wraps (pool is sized >= max expected alive, so wraps hit dead
// slots). Velocities: random unit-sphere direction biased along the burst normal (gNormalBias 0 =
// isotropic puff, 1 = fully directional along gNormal -- e.g. a contact normal).
struct Particle3D {
    float3 position; float size;
    float3 velocity; float lifetime;
    float  age; uint isActive; float2 _pad;
    float4 color;
};
RWStructuredBuffer<Particle3D> ParticleBuffer : register(u0);

cbuffer BurstCB : register(b0) {
    float3 gPos;        uint  gStartSlot;
    float3 gNormal;     uint  gCount;
    uint   gPoolSize;   float gSpeed; float gLifetime; float gSize;
    float4 gColor;
    float  gNormalBias; uint gSeed; float2 _p;
};

float hash11(uint n) { n = (n ^ 61u) ^ (n >> 16); n *= 9u; n ^= n >> 4; n *= 0x27d4eb2du; n ^= n >> 15; return (n & 0xffffffu) / 16777216.0f; }

[numthreads(64, 1, 1)]
void main(uint3 DTid : SV_DispatchThreadID) {
    uint i = DTid.x;
    if (i >= gCount) return;

    // Random point on the unit sphere (uniform via z + azimuth), then lerp toward the burst normal.
    float r1 = hash11(i * 3u + gSeed + 1u);
    float r2 = hash11(i * 7u + gSeed + 2u);
    float r3 = hash11(i * 11u + gSeed + 3u);
    float z   = r1 * 2.0f - 1.0f;
    float phi = r2 * 6.28318530718f;
    float s   = sqrt(max(0.0f, 1.0f - z * z));
    float3 dir = normalize(float3(s * cos(phi), s * sin(phi), z) * (1.0f - gNormalBias) + gNormal * gNormalBias);

    Particle3D p;
    p.position = gPos;
    p.velocity = dir * (gSpeed * (0.5f + r3));          // 0.5x..1.5x speed jitter
    p.lifetime = gLifetime * (0.6f + 0.8f * r1);
    p.age      = 0.0f;
    p.isActive = 1u;
    p.size     = gSize;
    p._pad     = float2(0, 0);
    p.color    = gColor;
    ParticleBuffer[(gStartSlot + i) % gPoolSize] = p;
}
