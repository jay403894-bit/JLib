// Shared by every per-effect update shader (UpdateParticles.hlsl, WaveParticles.hlsl, future
// ones) AND SpawnParticles.hlsl -- all of them bind through the SAME root signature (see
// Renderer::CreateComputeRootSignature), so they must all agree on this exact layout.
struct Particle2D
{
    float2 position;
    float2 velocity;
    float lifetime;
    // Seconds since this particle last (re)spawned -- lets per-particle behavior branch on how
    // long it's been alive (e.g. "age < 1: burst, else: drift") without needing a separate field
    // per phase.
    float age;
    uint isActive; // 1 if alive, 0 if dead
    float size;    // quad side length in pixels -- see ParticleVS.hlsl
    float4 color; // must stay in sync with ParticleVS.hlsl/RendererCore.h's Particle2D
};

// u0: this pool's own particle buffer. u1: this pool's own dead-list free list + hidden UAV
// counter (CounterOffsetInBytes, set up in Renderer::RegisterParticleEffect). An UPDATE shader
// only ever PUSHES onto DeadList (IncrementCounter -- Append semantics); SpawnParticles.hlsl is
// the only one that POPS (DecrementCounter -- Consume semantics) -- HLSL forbids doing both in
// one shader, which is why spawning is a separate dispatch/PSO even though it's the same buffer.
// u2: this pool's compacted list of ALIVE particle indices, rebuilt every frame by
// CompactParticles.hlsl (Append semantics via IncrementCounter) BEFORE the update dispatch runs.
// Update shaders index through THIS instead of raw DTid.x, so the update dispatch (sized off
// u2's counter via ExecuteIndirect, see RendererCore::UpdateParticles) only ever spends threads
// on particles that are actually alive, no matter where their slots land in ParticleBuffer.
// u3: a RAW, uncounted view over the SAME resource as u2 -- lets a shader read the alive count as
// a plain uint (AliveCounterRaw.Load) without touching u2's Increment/DecrementCounter semantics.
RWStructuredBuffer<Particle2D> ParticleBuffer : register(u0);
RWStructuredBuffer<uint> DeadList : register(u1);
RWStructuredBuffer<uint> AliveList : register(u2);
RWByteAddressBuffer AliveCounterRaw : register(u3);

// Root constants (see CreateComputeRootSignature): DeltaTime is the SAME dt main.cpp's
// GetFrameTime() computed this frame (real elapsed time, not a 60fps assumption). spawnCount is
// only meaningful to SpawnParticles.hlsl. maxParticles is this DISPATCH's pool size -- a runtime
// value now (not a shader constant) so one compiled update shader works correctly no matter
// which pool's buffers it's currently bound to. aliveListCounterOffset is u2/u3's hidden
// counter's byte offset within that resource (AlignUavCounterOffset-rounded, differs per pool
// since it depends on maxParticles) -- needed by AliveCounterRaw.Load below. GravityScale is a
// per-POOL px/s^2 constant (RendererCore::RegisterParticleEffect, default 0 = no gravity) --
// not baked into UpdateParticles.hlsl directly, since not every effect using that shader wants
// to fall (e.g. a screen-space overlay effect).
cbuffer TimeCB : register(b0)
{
    float DeltaTime;
    uint spawnCount;
    uint maxParticles;
    uint aliveListCounterOffset;
    float GravityScale;
};

// Common prologue for update shaders (NOT SpawnParticles.hlsl, which still scans by raw
// spawnCount/DTid.x): resolves DTid.x through the compacted alive list and bounds-checks against
// the CURRENT alive count, returning true if this thread has no work (caller should just return).
bool ResolveAliveIndex(uint threadId, out uint particleIndex)
{
    uint aliveCount = AliveCounterRaw.Load(aliveListCounterOffset);
    if (threadId >= aliveCount) { particleIndex = 0; return false; }
    particleIndex = AliveList[threadId];
    return true;
}

// Marks particle i dead and pushes its slot onto the free list for a future spawn to claim.
void KillParticle(uint i)
{
    ParticleBuffer[i].isActive = 0;
    // IncrementCounter() returns the PRE-increment count -- the slot to push this index into --
    // then bumps the counter so SpawnParticles.hlsl's DecrementCounter() can pop it.
    DeadList[DeadList.IncrementCounter()] = i;
}
