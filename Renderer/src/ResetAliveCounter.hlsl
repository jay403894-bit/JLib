// Single-thread: zeroes u2/u3's (AliveList's) hidden UAV counter before CompactParticles.hlsl
// re-appends this frame's alive indices. Separate dispatch (not folded into CompactParticles)
// because the reset must happen-before every append, and there's no ordering guarantee between
// threads within one dispatch without this being its own barrier-separated pass.
#include "ParticleCommon.hlsli"

[numthreads(1, 1, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    AliveCounterRaw.Store(aliveListCounterOffset, 0);
}
