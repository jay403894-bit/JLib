#pragma once
#include <string>
#include <DirectXMath.h>
#include "Mesh.h"
#include "ResourceManager.h"

namespace JLib {
    class Renderer2D;

    enum class TextAlign {
        Left,
        Center,
        Right
    };

    // Thin drawing/layout wrapper around a ResourceManager-owned FontResource (glyphs, kerning,
    // atlas texture -- see ResourceManager::LoadFont/FontResource) -- Font itself no longer owns
    // any of that data. m_Font is resolved once in Load() and cached as a raw pointer rather than
    // re-resolved on every draw: safe because FontResource's slot address never moves and is
    // never invalidated (fonts are never Unload()'d -- see ResourceManager.h's m_Fonts comment).
    class Font {
    public:
        void Load(const std::wstring& fntPath, const std::wstring& atlasPath, Renderer2D& renderer);

        // Main draw function - now with alignment and line support
        void SubmitText(Renderer2D& renderer, const std::string& text, float x, float y, float scale = 1.0f,
            DirectX::XMFLOAT4 color = { 1,1,1,1 }, float rotation = 0.0f,
            TextAlign align = TextAlign::Left, int zLayer = 2);

        float TextWidth(const std::string& text, float scale = 1.0f) const;
        float LineHeight() const { return m_Font ? m_Font->lineHeight : 0.0f; }

    private:
        void SubmitLine(Renderer2D& renderer, const std::string& line, float x, float y, float scale,
            DirectX::XMFLOAT4 color, float rotation, int zLayer);

        float Kerning(int first, int second) const;

        JLib::AssetHandle<FontResource> m_Handle;
        const FontResource* m_Font = nullptr;
        Mesh unitQuad{};
    };
};
