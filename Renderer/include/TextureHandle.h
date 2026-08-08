// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Joshua Makler. Part of JLib -- see LICENSE at the repository root.

#pragma once
#include <cstdint>

namespace JLib {
    // A lightweight, copyable handle to a texture owned by ResourceManager. Extracted into its own
    // header (out of ResourceManager.h) so low-level types like Mesh/Material can name a texture
    // WITHOUT pulling in the whole ResourceManager -- which itself includes Mesh.h, so Mesh.h
    // including ResourceManager.h would be a circular include. Decouples every caller from
    // TextureResource's layout/lifetime: ResourceManager is free to change how it stores textures
    // internally without any client holding a stale reference.
    struct TextureHandle {
        uint32_t id = UINT32_MAX;
        bool IsValid() const { return id != UINT32_MAX; }
        bool operator==(const TextureHandle& other) const { return id == other.id; }
    };
}
