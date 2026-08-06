#pragma once
#include <Windows.h>
#include <DirectXMath.h>
#include <vector>
#include <cstdint>

namespace JLib {
    // Keyboard/mouse AND gamepads via RAW INPUT (WM_INPUT), with XInput as an OPT-IN upgrade. Chosen
    // over GameInput for a hard requirement: ZERO threads of our own, and a thread count we can PROVE
    // rather than guess. GameInput spawns an undocumented, machine-varying number of internal threads
    // (and XInput spawns its own too), which makes exact core accounting impossible for a hard-pinned
    // scheduler (see JLib::TaskScheduler::GetSafeTC) -- "works on my machine" is not something a
    // library can ship. Here:
    //   * Keyboard/mouse: Raw Input on the app's EXISTING message pump (WM_INPUT -> OnRawInput).
    //   * Gamepads (default): Raw Input HID (usage page 1, usages 4/5) on the SAME pump -- reports are
    //     parsed with HidP_* (hid.dll, pure user-mode) into the standard layout below. Zero threads.
    //     Fidelity caveat on Xbox pads over HID: the XUSB driver reports BOTH triggers as one combined
    //     axis (split heuristically here), no rumble, no Guide button -- that's XInput's whole reason
    //     to exist. Sticks/buttons/dpad are full fidelity.
    //   * XInput (opt-in, lazy-loaded): polled in Update() for apps that want split triggers + rumble
    //     on Xbox pads and accept XInput's own threads as a KNOWN, opted-in cost. When enabled, XUSB
    //     devices are filtered out of the HID path (the "IG_" device-path marker) so a pad never
    //     reports twice.
    // Net: keyboard/mouse/gamepads contribute exactly 0 threads unless XInput is explicitly enabled.
    //
    // MOUSE DELTA SEMANTICS CHANGED vs the GameInput version: GetMouseDeltaX/Y now returns a TRUE
    // PER-FRAME delta (Raw Input reports relative motion natively). The old implementation returned a
    // CUMULATIVE accumulator that every caller had to difference by hand -- delete that differencing.
    class InputManager
    {
    public:
        ~InputManager();

        // hwnd = the game window. Raw Input is registered against it, so keyboard/mouse arrive only
        // while it has focus (standard game behavior -- no background input, no INPUTSINK hack).
        //
        // When (if ever) to fall back to XInput for pads whose driver merges the triggers.
        //
        // The DEFAULT is MergedTriggerPadsOnly, i.e. this IS automatic -- deliberately. A player whose
        // LT+RT cancel each other out has a visibly broken controller and no way to fix it; XInput's
        // threads are invisible and cost microseconds. Requiring every app author to predict "some of
        // my players own XUSB pads" gets that trade backwards. Correct by default, opt OUT for zero
        // threads -- not opt IN for working hardware.
        enum class XInputPolicy : uint8_t {
            // DEFAULT. Zero threads UNLESS a merged-trigger pad actually shows up: XInput is loaded
            // lazily, at the moment such a device is enumerated, and only that pad routes through it.
            // A player on a DualSense/Switch/modern Xbox pad pays nothing at all; a player on an XUSB
            // pad pays XInput's threads and gets working split triggers + rumble. Cost follows the
            // hardware that needs it.
            MergedTriggerPadsOnly = 0,
            // Opt OUT: zero threads, guaranteed, on every machine. Every pad is read via HID, and
            // pads bound to the XUSB driver (Xbox 360 AND third-party controllers in "XInput mode")
            // keep their MERGED trigger axis -- LT+RT held together cancel to neutral, indistinguishable
            // from "neither pressed". Driver-level, measured, unfixable in user space. Choose this if
            // your game never needs simultaneous triggers (a platformer, a puzzle game) and you want
            // the thread census to be exactly workers + main on every machine.
            Never,
            // Always load XInput; XUSB pads route through it, everything else stays on HID.
            Always,
        };

        // hwnd = the game window. Raw Input is registered against it, so input arrives only while it
        // has focus (standard game behavior -- no background input, no INPUTSINK hack).
        bool Initialize(HWND hwnd, XInputPolicy policy = XInputPolicy::MergedTriggerPadsOnly);

        // Load XInput now (e.g. behind a settings toggle). Returns false if no XInput DLL exists.
        // NOTE: unloading is impossible -- Windows gives no way to retire the threads a loaded
        // XInput created -- so disabling only stops polling. That asymmetry is exactly why the
        // default policy never loads it in the first place.
        bool EnableXInput(bool on);
        bool IsXInputEnabled() const { return m_XInputEnabled; }
        // Idempotent; safe to call twice or after a failed Initialize.
        void Shutdown();

        // Called from the window procedure for WM_INPUT (see Window::SetRawInputHandler). Decodes one
        // raw packet into the pending keyboard/mouse state. Runs on the message-pump thread -- the same
        // thread that calls Update() -- so no synchronization is needed anywhere in this class.
        void OnRawInput(LPARAM lParam);

        // Call once per frame BEFORE querying state: snapshots the pending raw-input accumulation into
        // "this frame", diffs against last frame for Pressed/Released edges, and polls XInput pads.
        void Update();

        bool IsKeyDown(uint8_t virtualKey) const;
        bool IsKeyPressed(uint8_t virtualKey) const;  // true only on the frame it transitions down
        bool IsKeyReleased(uint8_t virtualKey) const; // true only on the frame it transitions up

        // "Which key, if any, was just pressed" -- for menu/rebind UI where the caller doesn't know the
        // virtual-key code up front. Returns the FIRST newly-pressed key this frame via outKey.
        bool GetAnyKeyPressed(uint8_t& outKey) const;
        // All keys that newly transitioned down this frame (chord detection). Empty if none.
        std::vector<uint8_t> GetKeysPressedThisFrame() const;

        // Cursor position in CLIENT pixels (GetCursorPos + ScreenToClient) -- "where is the cursor".
        DirectX::XMFLOAT2 GetMousePos() const { return m_MousePos; }
        // TRUE per-frame relative motion (raw counts), zero when the mouse didn't move. See the class
        // note: this is NOT the old cumulative accumulator -- callers must NOT difference it.
        float GetMouseDeltaX() const { return (float)m_MouseDeltaX; }
        float GetMouseDeltaY() const { return (float)m_MouseDeltaY; }
        // Wheel notches this frame (+ = away from user), already divided by WHEEL_DELTA.
        float GetMouseWheelDelta() const { return m_MouseWheelDelta; }

        // Mouse buttons. Bit flags so callers can OR them; values chosen to match the old GameInput
        // enum's bit layout, so existing `MouseLeftButton`-style call sites only need the name changed.
        enum MouseButtons : uint32_t {
            MouseNone         = 0x00,
            MouseLeftButton   = 0x01,
            MouseRightButton  = 0x02,
            MouseMiddleButton = 0x04,
            MouseButton4      = 0x08,
            MouseButton5      = 0x10,
        };
        bool IsMouseButtonDown(MouseButtons button) const;
        bool IsMouseButtonPressed(MouseButtons button) const;  // true only on the click frame
        bool IsMouseButtonReleased(MouseButtons button) const; // true only on the release frame

        // Gamepad buttons -- values ARE the XInput bit masks (XINPUT_GAMEPAD_*), so the mapping is
        // identity and adding a button later is a one-line change.
        enum GamepadButtons : uint32_t {
            GamepadNone          = 0x0000,
            GamepadDPadUp        = 0x0001,
            GamepadDPadDown      = 0x0002,
            GamepadDPadLeft      = 0x0004,
            GamepadDPadRight     = 0x0008,
            GamepadMenu          = 0x0010,   // Start
            GamepadView          = 0x0020,   // Back/Select
            GamepadLeftThumb     = 0x0040,
            GamepadRightThumb    = 0x0080,
            GamepadLeftShoulder  = 0x0100,
            GamepadRightShoulder = 0x0200,
            GamepadA             = 0x1000,
            GamepadB             = 0x2000,
            GamepadX             = 0x4000,
            GamepadY             = 0x8000,
        };

        // Single-controller convenience -- operates on gamepad index 0.
        bool IsButtonDown(GamepadButtons button) const { return IsButtonDown(0, button); }
        bool IsButtonPressed(GamepadButtons button) const { return IsButtonPressed(0, button); }
        bool IsButtonReleased(GamepadButtons button) const { return IsButtonReleased(0, button); }
        float GetLeftTriggerAxis() const { return GetLeftTriggerAxis(0); }
        float GetRightTriggerAxis() const { return GetRightTriggerAxis(0); }
        float GetLeftStickX() const { return GetLeftStickX(0); }
        float GetLeftStickY() const { return GetLeftStickY(0); }
        float GetRightStickX() const { return GetRightStickX(0); }
        float GetRightStickY() const { return GetRightStickY(0); }

        // Multi-controller: gamepadIndex is the XInput user slot (0..3) -- a STABLE identity, so
        // "player 2 is slot 1" never silently re-points at a different pad after a hotplug. Out-of-range
        // or disconnected reads as all-neutral (buttons up, sticks/triggers 0) rather than throwing.
        bool IsButtonDown(uint32_t gamepadIndex, GamepadButtons button) const;
        bool IsButtonPressed(uint32_t gamepadIndex, GamepadButtons button) const;
        bool IsButtonReleased(uint32_t gamepadIndex, GamepadButtons button) const;
        float GetLeftTriggerAxis(uint32_t gamepadIndex) const;
        float GetRightTriggerAxis(uint32_t gamepadIndex) const;
        float GetLeftStickX(uint32_t gamepadIndex) const;
        float GetLeftStickY(uint32_t gamepadIndex) const;
        float GetRightStickX(uint32_t gamepadIndex) const;
        float GetRightStickY(uint32_t gamepadIndex) const;
        bool IsGamepadConnected(uint32_t gamepadIndex) const;
        uint32_t GetConnectedGamepadCount() const;

        // Vibration (0..1 per motor). No-op on a disconnected slot.
        void SetGamepadVibration(uint32_t gamepadIndex, float leftMotor, float rightMotor);

    private:
        static constexpr uint32_t kMaxGamepads = 4;   // XInput's hard limit
        HWND m_Hwnd = nullptr;
        bool m_Registered = false;

        // --- keyboard: 256 virtual-key slots, this frame vs last (edges are a diff, same as before) ---
        bool m_KeyDown[256] = {};       // live state, mutated by OnRawInput between Updates
        bool m_KeysThis[256] = {};      // snapshot for THIS frame's queries
        bool m_KeysPrev[256] = {};      // last frame's snapshot

        // --- mouse ---
        // Raw motion accumulates here between frames (several WM_INPUT packets can arrive per frame);
        // Update() moves it into m_MouseDelta* and zeroes it -- that is what makes the exposed delta a
        // true per-frame value rather than a running total.
        long  m_PendingDeltaX = 0, m_PendingDeltaY = 0;
        long  m_MouseDeltaX = 0, m_MouseDeltaY = 0;
        float m_PendingWheel = 0.0f, m_MouseWheelDelta = 0.0f;
        // Three states, same as the keyboard: live (mutated by OnRawInput between Updates), this
        // frame's snapshot, last frame's snapshot. Pressed/Released edges MUST diff the two
        // snapshots -- diffing live against a copy taken in the same Update() makes them identical
        // by query time and edges never fire (the launch bug that broke menu clicks).
        uint32_t m_MouseButtons = 0, m_MouseButtonsThis = 0, m_MouseButtonsPrev = 0;
        DirectX::XMFLOAT2 m_MousePos = { 0.0f, 0.0f };

        // --- gamepads ---
        // liveButtons is written by OnRawInput (HID packets arrive between frames) or by the XInput
        // poll; Update() snapshots it into buttons/prevButtons so Pressed/Released edges work the
        // same way the keyboard and mouse do. Axes need no edge detection, so they're written live.
        struct Pad {
            bool     connected = false;
            uint32_t liveButtons = 0;
            uint32_t buttons = 0, prevButtons = 0;
            float    leftTrigger = 0.0f, rightTrigger = 0.0f;
            float    lx = 0.0f, ly = 0.0f, rx = 0.0f, ry = 0.0f;
            HANDLE   hidDevice = nullptr;   // non-null => this slot is a HID pad
            int      xinputUser = -1;       // >=0   => this slot is XInput user N
        };
        Pad m_Pads[kMaxGamepads];
        // XInput is loaded DYNAMICALLY (never statically linked) so the DLL is absent from the process
        // -- and cannot create threads -- unless gamepads are explicitly enabled. Function pointers are
        // resolved once in EnableGamepads.
        bool  m_XInputEnabled = false;
        XInputPolicy m_XInputPolicy = XInputPolicy::MergedTriggerPadsOnly;
        HMODULE m_XInputDll = nullptr;
        using PFN_XInputGetState = DWORD(WINAPI*)(DWORD, void*);
        using PFN_XInputSetState = DWORD(WINAPI*)(DWORD, void*);
        PFN_XInputGetState m_XInputGetState = nullptr;
        PFN_XInputSetState m_XInputSetState = nullptr;
        // Polling an EMPTY XInput slot costs ~milliseconds (it probes the driver), so disconnected
        // slots are only re-probed every kEmptySlotPollFrames frames -- the standard mitigation for
        // XInput's most notorious performance trap. Connected slots poll every frame.
        static constexpr uint32_t kEmptySlotPollFrames = 90;
        uint32_t m_FrameCounter = 0;

        // --- HID gamepad decoding ---
        // Per-device parsing state, cached because re-fetching preparsed data per packet would be
        // absurd (a pad sends ~125 reports/sec). Keyed by the Raw Input device HANDLE.
        struct HidDevice {
            HANDLE                      hDevice = nullptr;
            std::vector<unsigned char>  preparsed;   // PHIDP_PREPARSED_DATA storage
            unsigned short              buttonPage = 0;
            unsigned short              buttonMin = 0, buttonMax = 0;
            // Axis ranges, resolved once from HIDP_VALUE_CAPS. The PAGE matters: sticks live on
            // Generic Desktop (0x01), but Xbox One/Series pads report their (separate!) triggers on
            // the Simulation Controls page (0x02) as Accelerator/Brake -- missing that page is why a
            // modern Xbox pad would otherwise fall back to the legacy combined-trigger path.
            struct Axis { unsigned short page = 0; unsigned short usage = 0; long lmin = 0; long lmax = 0; unsigned short bits = 0; };
            std::vector<Axis>           axes;
            int                         slot = -1;   // index into m_Pads
        };
        std::vector<HidDevice> m_HidDevices;
        // Finds (or lazily creates, on first packet) the parsing state for a device. Returns null if
        // the device isn't a usable gamepad or no free pad slot remains.
        HidDevice* AcquireHidDevice(HANDLE hDevice);
        // Decodes one HID input report into the device's pad slot.
        void DecodeHidReport(HidDevice& dev, const unsigned char* report, unsigned long len);
        // Registers usage-page-1 usages 4 (joystick) and 5 (gamepad) for Raw Input, and pre-populates
        // m_HidDevices from the already-connected device list so pads work before their first packet.
        void ScanHidGamepads();
    };
}
