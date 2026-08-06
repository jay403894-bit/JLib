#pragma once
#include <DirectXMath.h>
namespace JLib {
    // Plain data + two conversion functions -- deliberately NOT part of Renderer2D/RendererCore.
    // Renderer2D's Submit() already converts screen-space -> NDC per object on the CPU
    // (RendererCore::GetNDC), so a camera fits as an EXTRA transform the caller applies to world
    // coordinates before calling Submit(), not something baked into the renderer itself. This also
    // keeps the distinction between world-space sprites (should pan/zoom with the camera) and
    // screen-space UI/text (should NOT) explicit at each call site, instead of hidden behind a flag.
    class Camera2D
    {
    public:
        DirectX::XMFLOAT2 position = { 0.0f, 0.0f }; // world-space point the camera is centered on
        float zoom = 1.0f;                            // >1 = zoomed in, <1 = zoomed out

        // World-space position -> screen-space position (what you then pass to Renderer2D::Submit).
        // Centers the camera's `position` on the middle of the screen, then scales by `zoom`.
        DirectX::XMFLOAT2 WorldToScreen(DirectX::XMFLOAT2 worldPos, DirectX::XMFLOAT2 screenSize) const
        {
            DirectX::XMFLOAT2 screen;
            screen.x = (worldPos.x - position.x) * zoom + screenSize.x * 0.5f;
            screen.y = (worldPos.y - position.y) * zoom + screenSize.y * 0.5f;
            return screen;
        }

        // Inverse of WorldToScreen -- e.g. turning InputManager::GetMousePos() (screen-space) into a
        // world-space point for click-to-move/picking.
        DirectX::XMFLOAT2 ScreenToWorld(DirectX::XMFLOAT2 screenPos, DirectX::XMFLOAT2 screenSize) const
        {
            DirectX::XMFLOAT2 world;
            world.x = (screenPos.x - screenSize.x * 0.5f) / zoom + position.x;
            world.y = (screenPos.y - screenSize.y * 0.5f) / zoom + position.y;
            return world;
        }
    };
};