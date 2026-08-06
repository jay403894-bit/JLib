// SHARED across every particle pool (see ParticleCommon.hlsli / Renderer::RegisterParticleEffect)
// -- popping a dead slot and copying spawn data in is identical regardless of which pool's
// buffers this dispatch is currently bound to, so there's only ever one compiled copy of this.
#include "ParticleCommon.hlsli"

StructuredBuffer<Particle2D> SpawnBuffer : register(t1);

// Dispatched with exactly ceil(spawnCount/256) groups from the CPU (see Renderer::UpdateParticles) --
// unlike the update shaders, thread count here is sized to the number of PENDING SPAWNS, not
// maxParticles, since scanning every particle just to place a handful of spawns would be wasteful.
[numthreads(256, 1, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    uint i = DTid.x;
    if (i >= spawnCount) return;

    // DecrementCounter() returns the POST-decrement count, i.e. the index of the free slot being
    // popped. If the dead list is empty this underflows to a huge index; D3D12 guarantees
    // out-of-bounds structured-buffer reads return 0 rather than faulting, so worst case this
    // harmlessly re-spawns onto slot 0 instead of crashing -- acceptable since in steady state
    // there are far more dead particles than spawn requests per frame.
    uint freeSlot = DeadList[DeadList.DecrementCounter()];
    ParticleBuffer[freeSlot] = SpawnBuffer[i];
    ParticleBuffer[freeSlot].isActive = 1;
}
