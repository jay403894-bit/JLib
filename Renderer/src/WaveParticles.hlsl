// "Wave" behavior: drifts by velocity.x horizontally while oscillating vertically with a
// sin() driven by age -- a genuinely different compute shader from UpdateParticles.hlsl's
// straight-line motion, proving the per-effect-pool system actually swaps shaders, not branches.
// velocity.y is repurposed as the wave AMPLITUDE (pixels) for this behavior; velocity.x stays a
// normal horizontal speed.
#include "ParticleCommon.hlsli"

static const float WAVE_FREQUENCY_HZ = 2.0f;

[numthreads(256, 1, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    uint i;
    if (!ResolveAliveIndex(DTid.x, i)) return;

    ParticleBuffer[i].age += DeltaTime;
    float age = ParticleBuffer[i].age;

    float amplitude = ParticleBuffer[i].velocity.y;
    ParticleBuffer[i].position.x += ParticleBuffer[i].velocity.x * DeltaTime;
    // Absolute sin() of age (not an incremental += step) so the wave shape stays a clean
    // sine curve regardless of frame-time jitter -- base position is only known at spawn
    // time (velocity.x * age), so this reconstructs it each frame rather than drifting.
    ParticleBuffer[i].position.y += amplitude * sin(age * WAVE_FREQUENCY_HZ * 6.2831853f) * DeltaTime;

    ParticleBuffer[i].lifetime -= DeltaTime;
    if (ParticleBuffer[i].lifetime <= 0)
        KillParticle(i);
}
