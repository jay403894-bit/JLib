#include "../include/Font.h"
#include "../include/Renderer2D.h"
#include "../include/Helpers.h"
#include <sstream>
using namespace JLib;

void Font::Load(const std::wstring& fntPath, const std::wstring& atlasPath, Renderer2D& renderer) {
    ResourceManager* rm = renderer.GetResourceManager();

    // Atlas texture upload needs a command list -- same ExecuteUploadCommand idiom every other
    // texture load in this codebase uses (LoadTexture/CreateSolidColorTexture).
    renderer.ExecuteUploadCommand([&](ID3D12GraphicsCommandList* cmd) {
        m_Handle = rm->LoadFont(fntPath, atlasPath, cmd);
        });
    if (!m_Handle.IsValid()) {
        std::string p(fntPath.begin(), fntPath.end());
        throw std::runtime_error("Font::Load: cannot open '" + p + "'");
    }
    m_Font = &rm->ResolveFont(m_Handle);

    // ONE unit quad (-0.5..0.5, UV 0..1), shared by every glyph. Per-instance uvOffset/uvScale
    // (set in SubmitLine) selects which part of the atlas each instance actually samples --
    // same quad topology as main.cpp's moreVertices/quadIndices.
    Vertex quadVerts[] = {
        { -0.5f,  0.5f, 0.0f,  1,1,1,1,  0.0f, 0.0f }, // Top-Left
        {  0.5f,  0.5f, 0.0f,  1,1,1,1,  1.0f, 0.0f }, // Top-Right
        { -0.5f, -0.5f, 0.0f,  1,1,1,1,  0.0f, 1.0f }, // Bottom-Left
        {  0.5f, -0.5f, 0.0f,  1,1,1,1,  1.0f, 1.0f }, // Bottom-Right
    };
    uint32_t quadIndices[] = { 0, 1, 2, 1, 3, 2 };
    unitQuad = rm->CreateMesh(quadVerts, 4, quadIndices, 6);
}

float Font::Kerning(int first, int second) const {
    auto it = m_Font->kerning.find({ first, second });
    return it != m_Font->kerning.end() ? it->second : 0.0f;
}

void Font::SubmitText(Renderer2D& renderer, const std::string& text, float x, float y, float scale,
    DirectX::XMFLOAT4 color, float rotation, TextAlign align, int zLayer)
{
    if (!m_Font || !m_Font->texture.IsValid() || text.empty()) return;

    std::istringstream iss(text);
    std::string line;
    float currentY = y;

    while (std::getline(iss, line, '\n')) {
        float lineWidth = TextWidth(line, scale);
        float startX = x;

        if (align == TextAlign::Center) {
            startX = x - lineWidth * 0.5f;
        }
        else if (align == TextAlign::Left) {           // ← changed
            startX = x;                                // ← was Left, now Right

        }
        else if (align == TextAlign::Right) {          // ← changed
            startX = x - lineWidth;                    // ← was Right, now Left

        }

        SubmitLine(renderer, line, startX, currentY, scale, color, rotation, zLayer);
        currentY += m_Font->lineHeight * scale * 1.1f;
    }
}

void Font::SubmitLine(Renderer2D& renderer, const std::string& line, float x, float y, float scale,
    DirectX::XMFLOAT4 color, float rotation, int zLayer)
{
    float penX = 0.0f;
    int prev = -1;

    for (char c : line) {
        int id = static_cast<unsigned char>(c);
        auto it = m_Font->glyphs.find(id);
        if (it == m_Font->glyphs.end()) {
            if (id == ' ') {
                penX += 10.0f * scale; // crude space width
            }
            prev = id;
            continue;
        }

        const Glyph& g = it->second;

        if (prev >= 0) penX += Kerning(prev, id) * scale;

        if (g.width > 0 && g.height > 0) {
            float glyphW = g.width * scale;
            float glyphH = g.height * scale;

            float centerX = x + penX + g.xoffset * scale + glyphW * 0.5f;
            float centerY = y + g.yoffset * scale + glyphH * 0.5f;

            DirectX::XMFLOAT2 uvOffset{ g.x / m_Font->scaleW, g.y / m_Font->scaleH };
            DirectX::XMFLOAT2 uvScale{ g.width / m_Font->scaleW, g.height / m_Font->scaleH };
            BatchItem item;
            item.mesh = &unitQuad; // Font's own persistent member -- must NOT be a value copy (see SpriteBatchItem comment)
            item.tex = m_Font->texture;
            item.position = { centerX, centerY };;
            item.size = { glyphW, glyphH };
            item.zLayer = zLayer;
            item.color = color;
            item.rotation = rotation;
            item.uvOffset = uvOffset;
            item.uvScale = uvScale;
            item.useAlphaFromRGB = true; // BMFont atlases often have no alpha channel
            renderer.Submit(item);
        }

        penX += g.xadvance * scale;
        prev = id;
    }
}

float Font::TextWidth(const std::string& text, float scale) const {
    float width = 0.0f;
    int prev = -1;
    for (char c : text) {
        int id = static_cast<unsigned char>(c);
        auto it = m_Font->glyphs.find(id);
        if (it == m_Font->glyphs.end()) { prev = id; continue; }
        if (prev >= 0) width += Kerning(prev, id) * scale;
        width += it->second.xadvance * scale;
        prev = id;
    }
    return width;
}
