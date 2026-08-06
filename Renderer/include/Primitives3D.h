#pragma once
#include <vector>
#include <cmath>
#include "Vertex.h"           // Vertex3D
#include "Mesh.h"             // Mesh
#include "ResourceManager.h"  // CreateMesh<VertexT>

namespace JLib {

    // Builds a unit cube (side 1, centered on the origin: each coordinate is +/-0.5) as a Mesh.
    //
    // 24 vertices, NOT 8: a cube's 8 corners are each shared by 3 faces pointing 3 different ways,
    // but we want a single flat normal per face (so each face shades as one flat color). So every
    // face gets its OWN 4 vertices carrying that face's normal -> 6 faces * 4 = 24. 36 indices =
    // 6 faces * 2 triangles * 3 vertices. UVs are filled in for later texturing; the normal-color
    // pixel shader ignores them for now.
    inline Mesh MakeCubeMesh(ResourceManager& rm) {
        const Vertex3D verts[24] = {
            // +Z (front)                     normal            uv
            {{-0.5f,-0.5f, 0.5f}, {0,0, 1}, {0,1}},
            {{ 0.5f,-0.5f, 0.5f}, {0,0, 1}, {1,1}},
            {{ 0.5f, 0.5f, 0.5f}, {0,0, 1}, {1,0}},
            {{-0.5f, 0.5f, 0.5f}, {0,0, 1}, {0,0}},
            // -Z (back)
            {{ 0.5f,-0.5f,-0.5f}, {0,0,-1}, {0,1}},
            {{-0.5f,-0.5f,-0.5f}, {0,0,-1}, {1,1}},
            {{-0.5f, 0.5f,-0.5f}, {0,0,-1}, {1,0}},
            {{ 0.5f, 0.5f,-0.5f}, {0,0,-1}, {0,0}},
            // -X (left)
            {{-0.5f,-0.5f,-0.5f}, {-1,0,0}, {0,1}},
            {{-0.5f,-0.5f, 0.5f}, {-1,0,0}, {1,1}},
            {{-0.5f, 0.5f, 0.5f}, {-1,0,0}, {1,0}},
            {{-0.5f, 0.5f,-0.5f}, {-1,0,0}, {0,0}},
            // +X (right)
            {{ 0.5f,-0.5f, 0.5f}, {1,0,0},  {0,1}},
            {{ 0.5f,-0.5f,-0.5f}, {1,0,0},  {1,1}},
            {{ 0.5f, 0.5f,-0.5f}, {1,0,0},  {1,0}},
            {{ 0.5f, 0.5f, 0.5f}, {1,0,0},  {0,0}},
            // +Y (top)
            {{-0.5f, 0.5f, 0.5f}, {0,1,0},  {0,1}},
            {{ 0.5f, 0.5f, 0.5f}, {0,1,0},  {1,1}},
            {{ 0.5f, 0.5f,-0.5f}, {0,1,0},  {1,0}},
            {{-0.5f, 0.5f,-0.5f}, {0,1,0},  {0,0}},
            // -Y (bottom)
            {{-0.5f,-0.5f,-0.5f}, {0,-1,0}, {0,1}},
            {{ 0.5f,-0.5f,-0.5f}, {0,-1,0}, {1,1}},
            {{ 0.5f,-0.5f, 0.5f}, {0,-1,0}, {1,0}},
            {{-0.5f,-0.5f, 0.5f}, {0,-1,0}, {0,0}},
        };

        // Two triangles per face: (0,1,2) and (0,2,3), offset by 4 for each of the 6 faces.
        uint32_t indices[36];
        for (uint32_t f = 0; f < 6; ++f) {
            const uint32_t b = f * 4;
            const uint32_t o = f * 6;
            indices[o + 0] = b + 0; indices[o + 1] = b + 1; indices[o + 2] = b + 2;
            indices[o + 3] = b + 0; indices[o + 4] = b + 2; indices[o + 5] = b + 3;
        }

        return rm.CreateMesh<Vertex3D>(verts, 24, indices, 36);
    }

    // ---- shared helper for FACETED shapes (per-triangle flat normals, like the cube) ----
    // Appends triangle (a,b,c) with its computed face normal, auto-oriented to point AWAY from the
    // origin -- outward for these origin-centered convex shapes, and robust to winding mistakes since
    // the normal is what the shader shades by (culling is off for now). UV left (0,0): not textured yet.
    inline void addTri3D(std::vector<Vertex3D>& v, std::vector<uint32_t>& idx,
                         DirectX::XMFLOAT3 a, DirectX::XMFLOAT3 b, DirectX::XMFLOAT3 c) {
        float ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z;
        float wx = c.x - a.x, wy = c.y - a.y, wz = c.z - a.z;
        float nx = uy * wz - uz * wy, ny = uz * wx - ux * wz, nz = ux * wy - uy * wx;
        float len = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (len > 1e-8f) { nx /= len; ny /= len; nz /= len; }
        float cx = (a.x + b.x + c.x) / 3, cy = (a.y + b.y + c.y) / 3, cz = (a.z + b.z + c.z) / 3;
        if (nx * cx + ny * cy + nz * cz < 0) { nx = -nx; ny = -ny; nz = -nz; }   // face outward
        DirectX::XMFLOAT3 n{ nx, ny, nz };
        uint32_t base = (uint32_t)v.size();
        v.push_back({ a, n, {0.0f, 0.0f} });
        v.push_back({ b, n, {0.0f, 0.0f} });
        v.push_back({ c, n, {0.0f, 0.0f} });
        idx.push_back(base); idx.push_back(base + 1); idx.push_back(base + 2);
    }

    // Square-base pyramid, apex up. FACETED: 4 side triangles + 2 base triangles, each its own flat
    // normal (18 verts). Base +/-0.5 in XZ at y=-0.5, apex at (0, 0.5, 0).
    inline Mesh MakePyramidMesh(ResourceManager& rm) {
        std::vector<Vertex3D> v; std::vector<uint32_t> idx;
        const DirectX::XMFLOAT3 apex{ 0.0f, 0.5f, 0.0f };
        const DirectX::XMFLOAT3 b[4] = {
            {-0.5f,-0.5f,-0.5f}, {0.5f,-0.5f,-0.5f}, {0.5f,-0.5f,0.5f}, {-0.5f,-0.5f,0.5f} };
        for (int i = 0; i < 4; ++i) addTri3D(v, idx, apex, b[i], b[(i + 1) % 4]);   // 4 sides
        addTri3D(v, idx, b[0], b[1], b[2]);                                         // base (2 tris)
        addTri3D(v, idx, b[0], b[2], b[3]);
        return rm.CreateMesh<Vertex3D>(v.data(), (uint32_t)v.size(), idx.data(), (uint32_t)idx.size());
    }

    // Cone: circular base + apex. FACETED. Radius 0.5, base at y=-0.5, apex (0, 0.5, 0). `segments`
    // controls roundness.
    inline Mesh MakeConeMesh(ResourceManager& rm, int segments = 24) {
        std::vector<Vertex3D> v; std::vector<uint32_t> idx;
        const float R = 0.5f, TAU = 6.28318530717959f;
        const DirectX::XMFLOAT3 apex{ 0.0f, 0.5f, 0.0f }, baseC{ 0.0f, -0.5f, 0.0f };
        for (int i = 0; i < segments; ++i) {
            float a0 = TAU * i / segments, a1 = TAU * (i + 1) / segments;
            DirectX::XMFLOAT3 p0{ R * std::cos(a0), -0.5f, R * std::sin(a0) };
            DirectX::XMFLOAT3 p1{ R * std::cos(a1), -0.5f, R * std::sin(a1) };
            addTri3D(v, idx, apex, p0, p1);      // side slice
            addTri3D(v, idx, baseC, p0, p1);     // base fan slice
        }
        return rm.CreateMesh<Vertex3D>(v.data(), (uint32_t)v.size(), idx.data(), (uint32_t)idx.size());
    }

    // UV sphere. SMOOTH: shared verts, normal = normalized position. Radius 0.5. rings = latitude
    // stacks, sectors = longitude slices.
    inline Mesh MakeSphereMesh(ResourceManager& rm, int rings = 16, int sectors = 24) {
        std::vector<Vertex3D> v; std::vector<uint32_t> idx;
        const float R = 0.5f, PI = 3.14159265358979f, TAU = 6.28318530717959f;
        for (int i = 0; i <= rings; ++i) {
            float phi = PI * i / rings;                     // 0 (top) .. PI (bottom)
            float y = R * std::cos(phi), rr = R * std::sin(phi);
            for (int j = 0; j <= sectors; ++j) {
                float th = TAU * j / sectors;
                float x = rr * std::cos(th), z = rr * std::sin(th);
                v.push_back({ {x, y, z}, {x / R, y / R, z / R},
                              {(float)j / sectors, (float)i / rings} });   // normal = normalized pos
            }
        }
        const int stride = sectors + 1;
        for (int i = 0; i < rings; ++i)
            for (int j = 0; j < sectors; ++j) {
                uint32_t a = (uint32_t)(i * stride + j), b = a + stride;
                idx.push_back(a); idx.push_back(b); idx.push_back(a + 1);
                idx.push_back(a + 1); idx.push_back(b); idx.push_back(b + 1);
            }
        return rm.CreateMesh<Vertex3D>(v.data(), (uint32_t)v.size(), idx.data(), (uint32_t)idx.size());
    }

    // Capsule = cylinder + two hemisphere caps. SMOOTH. It's a UV sphere with the two halves pulled
    // apart by `cylHalf` along Y; the ring where they meet becomes the cylinder band (whose normals
    // are horizontal, exactly as a cylinder's should be). Radius R, caps centered at y = +/-cylHalf.
    inline Mesh MakeCapsuleMesh(ResourceManager& rm, int halfRings = 8, int sectors = 24) {
        std::vector<Vertex3D> v; std::vector<uint32_t> idx;
        const float R = 0.3f, cylHalf = 0.35f, PI = 3.14159265358979f, TAU = 6.28318530717959f;
        auto emitRing = [&](float phi, float yOff, float vCoord) {
            float ny = std::cos(phi), sinp = std::sin(phi);
            float ry = R * std::cos(phi) + yOff, rr = R * sinp;
            for (int j = 0; j <= sectors; ++j) {
                float th = TAU * j / sectors;
                v.push_back({ {rr * std::cos(th), ry, rr * std::sin(th)},
                              {sinp * std::cos(th), ny, sinp * std::sin(th)},
                              {(float)j / sectors, vCoord} });
            }
        };
        int totalRings = 0;
        for (int i = 0; i <= halfRings; ++i) {   // top hemisphere: phi 0..PI/2, offset +cylHalf
            emitRing(PI * 0.5f * i / halfRings, +cylHalf, 0.5f * i / halfRings); ++totalRings;
        }
        for (int i = 0; i <= halfRings; ++i) {   // bottom hemisphere: phi PI/2..PI, offset -cylHalf
            emitRing(PI * 0.5f + PI * 0.5f * i / halfRings, -cylHalf, 0.5f + 0.5f * i / halfRings); ++totalRings;
        }
        const int stride = sectors + 1;
        for (int i = 0; i < totalRings - 1; ++i) // the top-equator -> bottom-equator band IS the cylinder
            for (int j = 0; j < sectors; ++j) {
                uint32_t a = (uint32_t)(i * stride + j), b = a + stride;
                idx.push_back(a); idx.push_back(b); idx.push_back(a + 1);
                idx.push_back(a + 1); idx.push_back(b); idx.push_back(b + 1);
            }
        return rm.CreateMesh<Vertex3D>(v.data(), (uint32_t)v.size(), idx.data(), (uint32_t)idx.size());
    }

    // Cylinder: a tube with FLAT top + bottom caps. Radius 0.5, height 1 (y in +/-0.5) so it's unit-sized
    // like the cube/sphere (scale it by the body's half-extents to fit). SMOOTH side wall (radial normals)
    // + FLAT caps (+/-Y normals). Pairs with Physics3D::AddDynamicCylinder (Jolt's CylinderShape). Culling
    // is off, so winding is cosmetic -- shading uses the explicit normals set here. `sectors` = roundness.
    inline Mesh MakeCylinderMesh(ResourceManager& rm, int sectors = 24) {
        std::vector<Vertex3D> v; std::vector<uint32_t> idx;
        const float R = 0.5f, H = 0.5f, TAU = 6.28318530717959f;

        // Side wall: a top ring + bottom ring with radial normals; two triangles per sector between them.
        uint32_t side = (uint32_t)v.size();
        for (int j = 0; j <= sectors; ++j) {
            float th = TAU * j / sectors, cx = std::cos(th), sz = std::sin(th);
            DirectX::XMFLOAT3 n{ cx, 0.0f, sz };
            v.push_back({ { R * cx,  H, R * sz }, n, { (float)j / sectors, 0.0f } });   // top ring
            v.push_back({ { R * cx, -H, R * sz }, n, { (float)j / sectors, 1.0f } });   // bottom ring
        }
        for (int j = 0; j < sectors; ++j) {
            uint32_t a = side + j * 2;   // [a]=top j, [a+1]=bottom j, [a+2]=top j+1, [a+3]=bottom j+1
            idx.push_back(a); idx.push_back(a + 1); idx.push_back(a + 2);
            idx.push_back(a + 2); idx.push_back(a + 1); idx.push_back(a + 3);
        }

        // Flat caps: a center vertex + a ring fan, one Y-facing normal each.
        auto cap = [&](float y, float ny) {
            DirectX::XMFLOAT3 n{ 0.0f, ny, 0.0f };
            uint32_t c = (uint32_t)v.size();
            v.push_back({ { 0.0f, y, 0.0f }, n, { 0.5f, 0.5f } });
            uint32_t ring = (uint32_t)v.size();
            for (int j = 0; j <= sectors; ++j) {
                float th = TAU * j / sectors;
                v.push_back({ { R * std::cos(th), y, R * std::sin(th) }, n,
                              { 0.5f + 0.5f * std::cos(th), 0.5f + 0.5f * std::sin(th) } });
            }
            for (int j = 0; j < sectors; ++j) { idx.push_back(c); idx.push_back(ring + j); idx.push_back(ring + j + 1); }
        };
        cap( H,  1.0f);   // top
        cap(-H, -1.0f);   // bottom

        return rm.CreateMesh<Vertex3D>(v.data(), (uint32_t)v.size(), idx.data(), (uint32_t)idx.size());
    }
}
