// SHARED across every pool -- scanning maxParticles slots and appending the alive ones is
// identical regardless of pool behavior. This is the cheap O(maxParticles) pass that makes the
// expensive per-particle UPDATE dispatch (UpdateParticles.hlsl/WaveParticles.hlsl) only need to
// cover the actual alive count via ExecuteIndirect, instead of scanning the whole sparse buffer.
// Must run AFTER ResetAliveCounter.hlsl's dispatch (same frame, same pool) so AliveList's counter
// starts at 0 before any IncrementCounter here.
#include "ParticleCommon.hlsli"

[numthreads(256, 1, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    uint i = DTid.x;
    if (i >= maxParticles) return;

    if (ParticleBuffer[i].isActive == 1)
    {
        AliveList[AliveList.IncrementCounter()] = i;
    }
}
