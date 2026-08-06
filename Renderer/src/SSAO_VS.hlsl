// Fullscreen triangle, generated from the vertex ID alone -- no vertex or index buffer is bound for
// this pass (draw it with DrawInstanced(3, 1, 0, 0)). One oversized triangle rather than two quad
// triangles: the quad's shared diagonal gets rasterized twice and breaks up the 2x2 quads that ddx/ddy
// depend on, and SSAO_PS reconstructs its normals from exactly those derivatives.
struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VSOut VSMain(uint vid : SV_VertexID) {
    VSOut o;
    // vid 0 -> (0,0), 1 -> (2,0), 2 -> (0,2): UVs run past 1 so the triangle covers the whole screen.
    o.uv  = float2((vid << 1) & 2, vid & 2);
    // UV space (y down, 0..1) -> clip space (y up, -1..1).
    o.pos = float4(o.uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return o;
}
