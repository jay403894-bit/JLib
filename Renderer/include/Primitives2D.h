#pragma once
#include "Vertex.h"           // Vertex (2D: pos + rgba + uv)
#include "Mesh.h"             // Mesh
#include "ResourceManager.h"  // CreateMesh<VertexT>

namespace JLib {

    // The canonical 2D sprite quad: a unit quad (corners at +/-0.5) in the XY plane, white, standard
    // UVs, wound to match Renderer2D's sprite pipeline. EVERY sprite/particle references one of these
    // as its base mesh -- Renderer2D::Submit scales/positions/tints it per instance -- so centralize
    // it here instead of every client hand-copying the four verts. (Game-specific shapes, e.g. the
    // platformer's slope wedges, stay client-side; only universal primitives live in the lib.)
    //
    // Vertex color is white because per-sprite color comes from the instance data (BatchItem::color),
    // not the mesh; the quad just needs *some* valid color/uv for the input layout.
    inline Mesh MakeQuadMesh(ResourceManager& rm) {
        const Vertex verts[4] = {
            { -0.5f,  0.5f, 0.0f,  1.0f, 1.0f, 1.0f, 1.0f,  0.0f, 0.0f }, // top-left
            {  0.5f,  0.5f, 0.0f,  1.0f, 1.0f, 1.0f, 1.0f,  1.0f, 0.0f }, // top-right
            { -0.5f, -0.5f, 0.0f,  1.0f, 1.0f, 1.0f, 1.0f,  0.0f, 1.0f }, // bottom-left
            {  0.5f, -0.5f, 0.0f,  1.0f, 1.0f, 1.0f, 1.0f,  1.0f, 1.0f }, // bottom-right
        };
        const uint32_t indices[6] = { 0, 1, 2, 1, 3, 2 };
        return rm.CreateMesh(verts, 4, indices, 6);   // deduces VertexT = Vertex
    }
}
