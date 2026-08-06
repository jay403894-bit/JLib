// Single-thread dispatch that turns "how many particles are in the compacted alive list right
// now" into "how many thread groups does the update dispatch actually need" -- entirely on the
// GPU, so ExecuteIndirect never needs a CPU readback. Must run AFTER CompactParticles.hlsl's
// dispatch (same frame, same pool), since it reads the count CompactParticles just built.
//
// u0: RAW view over the pool's aliveListBuffer, covering its hidden UAV counter (the exact count
// CompactParticles.hlsl just appended). Deliberately a SEPARATE, uncounted view of the SAME
// resource the counted view (AliveList, u2 in the main compute root sig) uses.
// u1: RAW view over the pool's argsBuffer -- 3 UINTs, laid out exactly as D3D12_DISPATCH_ARGUMENTS
// (ThreadGroupCountX/Y/Z), which is what ExecuteIndirect reads directly off the GPU.
RWByteAddressBuffer AliveCounterRaw : register(u0);
RWByteAddressBuffer ArgsBuffer      : register(u1);

cbuffer ArgsConstants : register(b0)
{
    uint aliveListCounterOffset; // byte offset of the counter within AliveCounterRaw
};

[numthreads(1, 1, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    uint aliveCount = AliveCounterRaw.Load(aliveListCounterOffset);
    uint groupCount = (aliveCount + 255) / 256;
    // groupCount == 0 is a valid, useful case: ExecuteIndirect just dispatches nothing that frame.
    ArgsBuffer.Store3(0, uint3(groupCount, 1, 1));
}
