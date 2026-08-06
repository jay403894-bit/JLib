#pragma once
#include <windows.h>
#include <cstdint>
namespace JLib {
    class RendererCore; // <--- Forward declaration instead of #include -- Resize/VSync live on Core now
    class Window {
    public:
        // resizable=false drops WS_THICKFRAME/WS_MAXIMIZEBOX -- besides disabling drag-resize and
        // maximize, this ALSO removes Windows' system-enforced minimum resizable-window width
        // (SM_CXMIN), which a small fixed-layout window (e.g. a Tetris board sized to exactly
        // fill it) can otherwise be silently clamped above no matter what size is requested.
        Window(HINSTANCE hInstance, const wchar_t* title, int width, int height, bool resizable = true);
        ~Window();

        void Show();
        bool ProcessMessages(); // Replaces your while(PeekMessage) loop

        // The window forwards size changes to this renderer (set after construction).
        void SetRenderer(RendererCore* r) { m_renderer = r; }

        void SetFullscreen(bool fullscreen);   // borderless-fullscreen toggle (Alt+Enter / F11)

        HWND GetHandle() const { return m_hWnd; }
        // The REAL client area after window creation -- AdjustWindowRect (used internally to size
        // the window so the client area matches the constructor's width/height) computes border
        // sizes from system DPI, which can still drift from the actual per-monitor-DPI-aware
        // window on non-100%-scaled displays. Callers that need pixel-perfect sizing (anything
        // feeding RendererCore::Initialize, or content sized to exactly fill the window) should
        // use THIS instead of trusting the constructor's requested width/height blindly.
        void GetClientSize(uint32_t& width, uint32_t& height) const {
            RECT rect{};
            GetClientRect(m_hWnd, &rect);
            width = (uint32_t)(rect.right - rect.left);
            height = (uint32_t)(rect.bottom - rect.top);
        }

        // WM_INPUT forwarding hook. Raw Input (JLib::InputManager) is delivered as window messages, so
        // SOMETHING in the window procedure has to hand them over -- but the renderer must not depend on
        // the input library, so this is a plain function pointer any subsystem can install. Set it after
        // the window exists and before the first Update; pass nullptr to detach.
        using RawInputHandler = void(*)(void* user, LPARAM lParam);
        static void SetRawInputHandler(RawInputHandler fn, void* user) { s_RawInput = fn; s_RawInputUser = user; }

    private:
        static inline RawInputHandler s_RawInput = nullptr;
        static inline void*           s_RawInputUser = nullptr;
        static LRESULT CALLBACK WndProcSetup(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
        static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

        HWND m_hWnd;
        HINSTANCE m_hInstance;
        RendererCore* m_renderer = nullptr;  // not owned; just notified of resizes
        bool m_fullscreen = false;
        RECT m_windowRect{};             // saved windowed-mode rect, to restore from fullscreen
        UINT m_baseStyle = WS_OVERLAPPEDWINDOW; // non-fullscreen style -- SetFullscreen restores THIS, not a hardcoded style
    };
};