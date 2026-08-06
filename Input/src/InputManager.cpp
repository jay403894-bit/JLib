#include "../include/InputManager.h"
#include <Xinput.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <cstring>
#include <cmath>

// hid.lib is pure user-mode HID *parsing* (HidP_*) -- it adds no threads, unlike XInput below.
#pragma comment(lib, "hid.lib")

// NOTE: XInput is deliberately NOT linked (no #pragma comment(lib, "Xinput.lib")). It is LoadLibrary'd
// on demand in EnableGamepads -- a statically-linked import would map the DLL into every process at
// startup, and XInput creates threads of its own, which would silently reintroduce exactly the
// unaccountable-thread problem that got GameInput removed. Keyboard/mouse must cost zero threads.

namespace JLib {

InputManager::~InputManager() { Shutdown(); }

bool InputManager::Initialize(HWND hwnd, XInputPolicy policy) {
    m_XInputPolicy = policy;
    if (!hwnd) return false;
    m_Hwnd = hwnd;

    // Usage page 0x01 (generic desktop), usages 0x02 = mouse, 0x06 = keyboard. NO flags: input is
    // delivered only while m_Hwnd is the foreground window, which is exactly what a game wants (no
    // RIDEV_INPUTSINK -- that would deliver keystrokes while the player is alt-tabbed into a browser).
    // Usages 0x04 (joystick) and 0x05 (gamepad) ride the SAME registration and the same WM_INPUT
    // pump as keyboard/mouse -- which is the whole point: pads cost zero additional threads.
    RAWINPUTDEVICE rid[4] = {};
    rid[0].usUsagePage = 0x01; rid[0].usUsage = 0x02; rid[0].dwFlags = 0; rid[0].hwndTarget = hwnd; // mouse
    rid[1].usUsagePage = 0x01; rid[1].usUsage = 0x06; rid[1].dwFlags = 0; rid[1].hwndTarget = hwnd; // keyboard
    rid[2].usUsagePage = 0x01; rid[2].usUsage = 0x04; rid[2].dwFlags = 0; rid[2].hwndTarget = hwnd; // joystick
    rid[3].usUsagePage = 0x01; rid[3].usUsage = 0x05; rid[3].dwFlags = 0; rid[3].hwndTarget = hwnd; // gamepad

    m_Registered = RegisterRawInputDevices(rid, 4, sizeof(RAWINPUTDEVICE)) != FALSE;
    if (!m_Registered) {
        OutputDebugStringA("[InputManager] RegisterRawInputDevices FAILED -- keyboard/mouse will be dead.\n");
        return false;
    }
    if (policy == XInputPolicy::Always) EnableXInput(true);
    // Enumerate AFTER the Always case so XUSB devices are correctly excluded from HID; under
    // MergedTriggerPadsOnly this scan is what discovers a merged pad and triggers the lazy load.
    ScanHidGamepads();   // pads already plugged in work from frame 1, not just after first input
    return true;
}

// Resolves parsing state for a HID device, creating it on first sight (hotplug needs no callback:
// an unknown handle simply appears on its first report). Returns null for non-gamepads, devices we
// can't parse, or when all pad slots are taken.
InputManager::HidDevice* InputManager::AcquireHidDevice(HANDLE hDevice) {
    for (auto& d : m_HidDevices)
        if (d.hDevice == hDevice) return &d;

    RID_DEVICE_INFO info{}; info.cbSize = sizeof(info);
    UINT sz = sizeof(info);
    if (GetRawInputDeviceInfoW(hDevice, RIDI_DEVICEINFO, &info, &sz) == (UINT)-1) return nullptr;
    if (info.dwType != RIM_TYPEHID) return nullptr;
    if (info.hid.usUsagePage != 0x01) return nullptr;
    const USHORT u = info.hid.usUsage;
    if (u != 0x04 && u != 0x05 && u != 0x08) return nullptr;   // joystick / gamepad / multi-axis

    // When XInput is ALSO enabled, skip XUSB devices here so an Xbox pad isn't reported twice
    // (the "IG_" marker in the device path is the documented way to detect an XInput device).
    if (m_XInputEnabled) {
        WCHAR path[512] = {}; UINT psz = 512;
        if (GetRawInputDeviceInfoW(hDevice, RIDI_DEVICENAME, path, &psz) != (UINT)-1 &&
            wcsstr(path, L"IG_") != nullptr)
            return nullptr;
    }

    UINT ppdSize = 0;
    if (GetRawInputDeviceInfoW(hDevice, RIDI_PREPARSEDDATA, nullptr, &ppdSize) == (UINT)-1 || ppdSize == 0)
        return nullptr;

    HidDevice dev;
    dev.hDevice = hDevice;
    dev.preparsed.resize(ppdSize);
    if (GetRawInputDeviceInfoW(hDevice, RIDI_PREPARSEDDATA, dev.preparsed.data(), &ppdSize) == (UINT)-1)
        return nullptr;
    auto ppd = reinterpret_cast<PHIDP_PREPARSED_DATA>(dev.preparsed.data());

    HIDP_CAPS caps{};
    if (HidP_GetCaps(ppd, &caps) != HIDP_STATUS_SUCCESS) return nullptr;

    // Buttons: take the first input button cap that is a RANGE (the standard "buttons 1..N" form).
    if (caps.NumberInputButtonCaps > 0) {
        std::vector<HIDP_BUTTON_CAPS> bc(caps.NumberInputButtonCaps);
        USHORT n = caps.NumberInputButtonCaps;
        if (HidP_GetButtonCaps(HidP_Input, bc.data(), &n, ppd) == HIDP_STATUS_SUCCESS) {
            for (USHORT i = 0; i < n; ++i) {
                if (!bc[i].IsRange) continue;
                dev.buttonPage = bc[i].UsagePage;
                dev.buttonMin  = bc[i].Range.UsageMin;
                dev.buttonMax  = bc[i].Range.UsageMax;
                break;
            }
        }
    }

    // Axes: cache usage + logical range. NOTE the unsigned-range quirk seen on the 360 pad --
    // descriptors express a 0..65535 range as LogicalMax = -1 (0xFFFF read as signed), so when max
    // <= min we reconstruct the true max from the field's bit width instead of trusting it.
    if (caps.NumberInputValueCaps > 0) {
        std::vector<HIDP_VALUE_CAPS> vc(caps.NumberInputValueCaps);
        USHORT n = caps.NumberInputValueCaps;
        if (HidP_GetValueCaps(HidP_Input, vc.data(), &n, ppd) == HIDP_STATUS_SUCCESS) {
            for (USHORT i = 0; i < n; ++i) {
                HidDevice::Axis a;
                a.page  = vc[i].UsagePage;
                a.usage = vc[i].IsRange ? vc[i].Range.UsageMin : vc[i].NotRange.Usage;
                a.bits  = vc[i].BitSize;
                a.lmin  = vc[i].LogicalMin;
                a.lmax  = vc[i].LogicalMax;
                if (a.lmax <= a.lmin)
                    a.lmax = (a.bits >= 32) ? 0x7FFFFFFF : ((1L << a.bits) - 1);
                dev.axes.push_back(a);
            }
        }
    }

    // MERGED-TRIGGER DETECTION -- the XUSB compatibility signature: a single Generic Desktop Z with
    // no Rz partner and no Simulation Controls Accelerator/Brake pair. True for Xbox 360 pads AND
    // for third-party controllers running in "XInput mode" (same driver, same merge).
    bool hasZ = false, hasRz = false, hasAccel = false, hasBrake = false;
    for (const auto& a : dev.axes) {
        if (a.page == 0x01 && a.usage == 0x32) hasZ = true;
        if (a.page == 0x01 && a.usage == 0x35) hasRz = true;
        if (a.page == 0x02 && a.usage == 0xC4) hasAccel = true;
        if (a.page == 0x02 && a.usage == 0xC5) hasBrake = true;
    }
    const bool mergedTriggers = hasZ && !hasRz && !(hasAccel && hasBrake);

    // Under MergedTriggerPadsOnly, THIS is the moment the cost is incurred: a merged pad appeared,
    // so load XInput and hand this device to it (returning null leaves it unclaimed by HID; the
    // XInput poll picks it up next Update). Players on pads with real split triggers never get here,
    // so they never pay for XInput's threads.
    if (mergedTriggers && m_XInputPolicy == XInputPolicy::MergedTriggerPadsOnly && !m_XInputEnabled) {
        if (EnableXInput(true)) {
            OutputDebugStringA("[InputManager] Merged-trigger (XUSB) pad detected -- XInput loaded "
                               "for split triggers/rumble, per XInputPolicy::MergedTriggerPadsOnly.\n");
            return nullptr;
        }
        // No XInput available: fall through and use HID anyway (merged triggers beats no pad).
    }

    for (uint32_t s = 0; s < kMaxGamepads; ++s) {
        if (m_Pads[s].connected) continue;
        dev.slot = (int)s;
        m_Pads[s] = Pad{};
        m_Pads[s].connected = true;
        m_Pads[s].hidDevice = hDevice;
        break;
    }
    if (dev.slot < 0) return nullptr;   // all four slots busy

    m_HidDevices.push_back(std::move(dev));
    return &m_HidDevices.back();
}

void InputManager::ScanHidGamepads() {
    UINT count = 0;
    if (GetRawInputDeviceList(nullptr, &count, sizeof(RAWINPUTDEVICELIST)) != 0 || count == 0) return;
    std::vector<RAWINPUTDEVICELIST> list(count);
    count = GetRawInputDeviceList(list.data(), &count, sizeof(RAWINPUTDEVICELIST));
    if (count == (UINT)-1) return;
    for (UINT i = 0; i < count; ++i)
        if (list[i].dwType == RIM_TYPEHID) AcquireHidDevice(list[i].hDevice);
}

// Default mapping, matching the near-universal Xbox-style HID layout (verified against a 360 pad:
// buttons 1..10 = A B X Y LB RB Back Start LThumb RThumb; X/Y = left stick, Rx/Ry = right stick,
// Z = COMBINED triggers, hat = dpad). Pads that deviate (DualShock button order, etc.) are what a
// GUID->layout mapping table (SDL_GameControllerDB format) would eventually correct; the parsing
// above is already mapping-agnostic, so adding that later touches only this function.
void InputManager::DecodeHidReport(HidDevice& dev, const unsigned char* report, unsigned long len) {
    if (dev.slot < 0) return;
    Pad& pad = m_Pads[dev.slot];
    auto ppd = reinterpret_cast<PHIDP_PREPARSED_DATA>(dev.preparsed.data());
    auto* raw = const_cast<PCHAR>(reinterpret_cast<const char*>(report));

    // ---- buttons ----
    uint32_t bits = 0;
    if (dev.buttonMax >= dev.buttonMin && dev.buttonMin != 0) {
        const ULONG maxCount = (ULONG)(dev.buttonMax - dev.buttonMin + 1);
        std::vector<USAGE> pressed(maxCount);
        ULONG n = maxCount;
        if (HidP_GetUsages(HidP_Input, dev.buttonPage, 0, pressed.data(), &n, ppd, raw, len)
                == HIDP_STATUS_SUCCESS) {
            for (ULONG i = 0; i < n; ++i) {
                switch (pressed[i] - dev.buttonMin) {   // 0-based button index
                    case 0: bits |= GamepadA;             break;
                    case 1: bits |= GamepadB;             break;
                    case 2: bits |= GamepadX;             break;
                    case 3: bits |= GamepadY;             break;
                    case 4: bits |= GamepadLeftShoulder;  break;
                    case 5: bits |= GamepadRightShoulder; break;
                    case 6: bits |= GamepadView;          break;   // Back/Select
                    case 7: bits |= GamepadMenu;          break;   // Start
                    case 8: bits |= GamepadLeftThumb;     break;
                    case 9: bits |= GamepadRightThumb;    break;
                    default: break;
                }
            }
        }
    }

    // ---- axes ----
    auto readAxis = [&](USHORT page, USHORT usage, float& out01, bool* found = nullptr) {
        for (const auto& a : dev.axes) {
            if (a.page != page || a.usage != usage) continue;
            ULONG value = 0;
            if (HidP_GetUsageValue(HidP_Input, page, 0, usage, &value, ppd, raw, len)
                    != HIDP_STATUS_SUCCESS) break;
            const float span = (float)(a.lmax - a.lmin);
            out01 = span > 0.0f ? ((float)((long)value - a.lmin) / span) : 0.5f;
            if (found) *found = true;
            return;
        }
        if (found) *found = false;
    };

    // Sticks: HID reports 0..1 with 0.5 centered; convert to -1..1, invert Y (HID Y grows DOWN,
    // every gameplay convention wants up-positive), then apply the same radial deadzone as XInput.
    float lx01 = 0.5f, ly01 = 0.5f, rx01 = 0.5f, ry01 = 0.5f;
    readAxis(0x01, 0x30, lx01);   // X
    readAxis(0x01, 0x31, ly01);   // Y
    readAxis(0x01, 0x33, rx01);   // Rx
    readAxis(0x01, 0x34, ry01);   // Ry

    auto applyStick = [](float x01, float y01, float& outX, float& outY) {
        float x = x01 * 2.0f - 1.0f;
        float y = -(y01 * 2.0f - 1.0f);
        float mag = sqrtf(x * x + y * y);
        const float dz = 0.24f;   // ~= XInput's 7849/32767 default, radial (no square notch)
        if (mag <= dz) { outX = 0.0f; outY = 0.0f; return; }
        if (mag > 1.0f) mag = 1.0f;
        const float norm = (mag - dz) / (1.0f - dz);
        const float inv = 1.0f / mag;
        outX = x * inv * norm;
        outY = y * inv * norm;
    };
    applyStick(lx01, ly01, pad.lx, pad.ly);
    applyStick(rx01, ry01, pad.rx, pad.ry);

    // Triggers, in descending order of fidelity. Three real-world layouts exist:
    //   (a) Simulation Controls page: Accelerator (0xC4) / Brake (0xC5) -- Xbox One & Series pads.
    //   (b) Generic Desktop Z (0x32) + Rz (0x35) as two axes -- DualShock/DualSense, most others.
    //   (c) Generic Desktop Z ALONE, pre-merged -- Xbox 360 via the XUSB compatibility interface.
    // (c) is not a parsing failure and no byte-offset trick can undo it: MEASURED on a 360 pad, LT
    // and RT cancel to the exact resting bytes when held together, so the driver destroys the two
    // values before the report is ever generated. Splitting them requires XInput (which is what it
    // exists for) -- or simply not designing controls that need LT+RT simultaneously.
    float accel01 = 0.0f, brake01 = 0.0f;
    bool haveAccel = false, haveBrake = false;
    readAxis(0x02, 0xC4, accel01, &haveAccel);
    readAxis(0x02, 0xC5, brake01, &haveBrake);

    float z01 = 0.5f, rz01 = 0.0f;
    bool haveZ = false, haveRz = false;
    readAxis(0x01, 0x32, z01,  &haveZ);
    readAxis(0x01, 0x35, rz01, &haveRz);

    if (haveAccel && haveBrake) {     // (a) modern Xbox -- full fidelity
        pad.rightTrigger = accel01;
        pad.leftTrigger  = brake01;
    } else if (haveZ && haveRz) {     // (b) two generic axes -- full fidelity
        pad.leftTrigger  = z01;
        pad.rightTrigger = rz01;
    } else if (haveZ) {               // (c) legacy merged single axis -- see above
        pad.leftTrigger  = z01 > 0.5f ? (z01 - 0.5f) * 2.0f : 0.0f;
        pad.rightTrigger = z01 < 0.5f ? (0.5f - z01) * 2.0f : 0.0f;
    }

    // ---- hat switch -> dpad ----
    for (const auto& a : dev.axes) {
        if (a.page != 0x01 || a.usage != 0x39) continue;
        ULONG value = 0;
        if (HidP_GetUsageValue(HidP_Input, 0x01, 0, 0x39, &value, ppd, raw, len) != HIDP_STATUS_SUCCESS)
            break;
        const long v = (long)value - a.lmin;   // 0..7 clockwise from up; anything else = centered
        if (v >= 0 && v <= 7) {
            static const uint32_t kHat[8] = {
                GamepadDPadUp,
                GamepadDPadUp   | GamepadDPadRight,
                GamepadDPadRight,
                GamepadDPadDown | GamepadDPadRight,
                GamepadDPadDown,
                GamepadDPadDown | GamepadDPadLeft,
                GamepadDPadLeft,
                GamepadDPadUp   | GamepadDPadLeft,
            };
            bits |= kHat[v];
        }
        break;
    }

    pad.liveButtons = bits;
}

bool InputManager::EnableXInput(bool on) {
    if (!on) { m_XInputEnabled = false; return true; }   // stop polling; DLL stays (see header)
    if (m_XInputGetState) { m_XInputEnabled = true; return true; }   // already loaded

    // Newest first: 1_4 (Win8+, in-box), 1_3 (legacy redist), 9_1_0 (ancient in-box fallback).
    const wchar_t* candidates[] = { L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll" };
    for (const wchar_t* name : candidates) {
        m_XInputDll = LoadLibraryW(name);
        if (m_XInputDll) break;
    }
    if (!m_XInputDll) {
        OutputDebugStringA("[InputManager] No XInput DLL found -- gamepads unavailable.\n");
        return false;
    }
    m_XInputGetState = (PFN_XInputGetState)GetProcAddress(m_XInputDll, "XInputGetState");
    m_XInputSetState = (PFN_XInputSetState)GetProcAddress(m_XInputDll, "XInputSetState");
    if (!m_XInputGetState) {
        FreeLibrary(m_XInputDll); m_XInputDll = nullptr;
        OutputDebugStringA("[InputManager] XInputGetState missing -- gamepads unavailable.\n");
        return false;
    }
    OutputDebugStringA("[InputManager] Gamepads ENABLED (XInput loaded -- it creates its own threads; "
                       "budget scheduler cores accordingly).\n");
    m_XInputEnabled = true;
    return true;
}

void InputManager::Shutdown() {
    if (m_Registered) {
        // RIDEV_REMOVE requires hwndTarget == nullptr (documented, and it fails otherwise).
        RAWINPUTDEVICE rid[2] = {};
        rid[0].usUsagePage = 0x01; rid[0].usUsage = 0x02; rid[0].dwFlags = RIDEV_REMOVE; rid[0].hwndTarget = nullptr;
        rid[1].usUsagePage = 0x01; rid[1].usUsage = 0x06; rid[1].dwFlags = RIDEV_REMOVE; rid[1].hwndTarget = nullptr;
        RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE));
        m_Registered = false;
    }
    // Stop any motors we left running -- a vibrating pad outlives the process otherwise.
    for (uint32_t i = 0; i < kMaxGamepads; ++i)
        if (m_Pads[i].connected) SetGamepadVibration(i, 0.0f, 0.0f);
    m_HidDevices.clear();
    for (uint32_t i = 0; i < kMaxGamepads; ++i) m_Pads[i] = Pad{};
    m_Hwnd = nullptr;
}

void InputManager::OnRawInput(LPARAM lParam) {
    // Fixed-size buffer sized for HID gamepad reports too (a 360 pad sends 15 bytes; DualSense
    // sends 64 over USB / 78 over Bluetooth -- 1 KB covers every consumer pad with huge margin, and
    // an oversized report simply fails the call below rather than overflowing). Avoids a per-message
    // heap allocation on the hottest message in the app.
    alignas(8) BYTE buffer[1024];
    UINT size = sizeof(buffer);
    if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, buffer, &size, sizeof(RAWINPUTHEADER)) == (UINT)-1)
        return;

    const RAWINPUT* raw = reinterpret_cast<const RAWINPUT*>(buffer);

    if (raw->header.dwType == RIM_TYPEKEYBOARD) {
        const RAWKEYBOARD& kb = raw->data.keyboard;
        if (kb.VKey >= 256 || kb.VKey == 0xFF) return;   // 0xFF = fake key from some multimedia devices
        const bool down = (kb.Flags & RI_KEY_BREAK) == 0;

        // Disambiguate the "generic" VKs into left/right where the scan code tells us, then ALSO set
        // the generic one -- so IsKeyDown(VK_SHIFT) and IsKeyDown(VK_LSHIFT) both work as expected.
        uint8_t vk = (uint8_t)kb.VKey;
        const bool e0 = (kb.Flags & RI_KEY_E0) != 0;
        switch (kb.VKey) {
            case VK_SHIFT:   vk = (uint8_t)MapVirtualKeyW(kb.MakeCode, MAPVK_VSC_TO_VK_EX); break;
            case VK_CONTROL: vk = e0 ? VK_RCONTROL : VK_LCONTROL; break;
            case VK_MENU:    vk = e0 ? VK_RMENU    : VK_LMENU;    break;
            default: break;
        }
        m_KeyDown[vk] = down;
        if (vk != kb.VKey) m_KeyDown[kb.VKey] = down;   // keep the generic VK in sync too
        return;
    }

    if (raw->header.dwType == RIM_TYPEMOUSE) {
        const RAWMOUSE& m = raw->data.mouse;

        // MOUSE_MOVE_ABSOLUTE comes from tablets/remote desktop/some VMs: lLastX/Y are then normalized
        // absolute coords, NOT deltas -- differencing them against the previous absolute gives the true
        // relative motion. Plain mice report relative already (the overwhelmingly common path).
        if ((m.usFlags & MOUSE_MOVE_ABSOLUTE) != 0) {
            static bool s_haveAbs = false;
            static long s_lastAbsX = 0, s_lastAbsY = 0;
            if (s_haveAbs) { m_PendingDeltaX += m.lLastX - s_lastAbsX; m_PendingDeltaY += m.lLastY - s_lastAbsY; }
            s_lastAbsX = m.lLastX; s_lastAbsY = m.lLastY; s_haveAbs = true;
        } else {
            m_PendingDeltaX += m.lLastX;
            m_PendingDeltaY += m.lLastY;
        }

        const USHORT bf = m.usButtonFlags;
        if (bf & RI_MOUSE_LEFT_BUTTON_DOWN)   m_MouseButtons |= MouseLeftButton;
        if (bf & RI_MOUSE_LEFT_BUTTON_UP)     m_MouseButtons &= ~(uint32_t)MouseLeftButton;
        if (bf & RI_MOUSE_RIGHT_BUTTON_DOWN)  m_MouseButtons |= MouseRightButton;
        if (bf & RI_MOUSE_RIGHT_BUTTON_UP)    m_MouseButtons &= ~(uint32_t)MouseRightButton;
        if (bf & RI_MOUSE_MIDDLE_BUTTON_DOWN) m_MouseButtons |= MouseMiddleButton;
        if (bf & RI_MOUSE_MIDDLE_BUTTON_UP)   m_MouseButtons &= ~(uint32_t)MouseMiddleButton;
        if (bf & RI_MOUSE_BUTTON_4_DOWN)      m_MouseButtons |= MouseButton4;
        if (bf & RI_MOUSE_BUTTON_4_UP)        m_MouseButtons &= ~(uint32_t)MouseButton4;
        if (bf & RI_MOUSE_BUTTON_5_DOWN)      m_MouseButtons |= MouseButton5;
        if (bf & RI_MOUSE_BUTTON_5_UP)        m_MouseButtons &= ~(uint32_t)MouseButton5;

        if (bf & RI_MOUSE_WHEEL)
            m_PendingWheel += (float)(short)m.usButtonData / (float)WHEEL_DELTA;
        return;
    }

    if (raw->header.dwType == RIM_TYPEHID) {
        // Gamepads. An unknown handle here IS the hotplug path -- AcquireHidDevice parses the
        // descriptor and claims a slot on first report, no device-notification plumbing needed.
        HidDevice* dev = AcquireHidDevice(raw->header.hDevice);
        if (!dev) return;
        // A HID packet may carry several stacked reports (dwCount); decode each.
        const DWORD stride = raw->data.hid.dwSizeHid;
        const DWORD n      = raw->data.hid.dwCount;
        const BYTE* base   = raw->data.hid.bRawData;
        for (DWORD i = 0; i < n; ++i)
            DecodeHidReport(*dev, base + (size_t)i * stride, stride);
    }
}

// Normalizes one XInput thumbstick axis pair into -1..1 with a RADIAL deadzone (magnitude-based, not
// per-axis): a per-axis deadzone leaves the classic square notch that makes diagonal movement snap.
static void NormalizeStick(SHORT sx, SHORT sy, SHORT deadzone, float& outX, float& outY) {
    float x = (float)sx, y = (float)sy;
    float mag = sqrtf(x * x + y * y);
    if (mag <= (float)deadzone) { outX = 0.0f; outY = 0.0f; return; }
    const float kMax = 32767.0f;
    if (mag > kMax) mag = kMax;
    // Rescale so the stick reaches full 1.0 at the edge despite the deadzone eating the inner ring.
    float norm = (mag - (float)deadzone) / (kMax - (float)deadzone);
    float invMag = 1.0f / mag;
    outX = x * invMag * norm;
    outY = y * invMag * norm;
}

void InputManager::Update() {
    // ---- keyboard: snapshot live state into this frame, keeping last frame for edge detection ----
    std::memcpy(m_KeysPrev, m_KeysThis, sizeof(m_KeysPrev));
    std::memcpy(m_KeysThis, m_KeyDown, sizeof(m_KeysThis));

    // Focus loss: Windows sends no key-up for keys still held when we lose focus, so they'd latch
    // "down" forever. Clear everything the moment we're not foreground (also stops charge/aim inputs
    // continuing while alt-tabbed).
    if (m_Hwnd && GetForegroundWindow() != m_Hwnd) {
        std::memset(m_KeyDown, 0, sizeof(m_KeyDown));
        std::memset(m_KeysThis, 0, sizeof(m_KeysThis));
        m_MouseButtons = 0;
        m_PendingDeltaX = m_PendingDeltaY = 0;
        m_PendingWheel = 0.0f;
    }

    // ---- mouse: snapshot buttons (prev <- last frame's snapshot, this <- live), publish the
    // accumulated raw motion as THIS frame's delta, then reset. The two-step snapshot is what makes
    // Pressed/Released edges work -- see the member comment in the header.
    m_MouseButtonsPrev = m_MouseButtonsThis;
    m_MouseButtonsThis = m_MouseButtons;
    m_MouseDeltaX = m_PendingDeltaX; m_PendingDeltaX = 0;
    m_MouseDeltaY = m_PendingDeltaY; m_PendingDeltaY = 0;
    m_MouseWheelDelta = m_PendingWheel; m_PendingWheel = 0.0f;

    POINT p{};
    if (GetCursorPos(&p) && m_Hwnd && ScreenToClient(m_Hwnd, &p))
        m_MousePos = { (float)p.x, (float)p.y };

    // ---- gamepads ----
    // HID pads are event-driven: OnRawInput already wrote liveButtons/axes between frames, so all
    // Update() owes them is the button snapshot that makes Pressed/Released edges work.
    for (uint32_t i = 0; i < kMaxGamepads; ++i) {
        if (!m_Pads[i].hidDevice) continue;
        m_Pads[i].prevButtons = m_Pads[i].buttons;
        m_Pads[i].buttons     = m_Pads[i].liveButtons;
    }

    // XInput pads (loaded only per XInputPolicy). Connected users poll every frame, absent users
    // only occasionally (see header). NOTE: XInput user index != pad slot -- HID pads may already
    // own low slots, so each user claims its own slot on first sight and keeps it. Assuming user N
    // owns slot N silently loses a pad whenever both backends are live at once.
    if (!m_XInputEnabled || !m_XInputGetState) return;
    ++m_FrameCounter;
    const bool probeEmpty = (m_FrameCounter % kEmptySlotPollFrames) == 0;
    for (uint32_t user = 0; user < kMaxGamepads; ++user) {
        int slot = -1;
        for (uint32_t s = 0; s < kMaxGamepads; ++s)
            if (m_Pads[s].xinputUser == (int)user) { slot = (int)s; break; }

        if (slot < 0 && !probeEmpty) continue;   // unknown user: only probe on the slow cadence

        XINPUT_STATE st{};
        const bool ok = m_XInputGetState(user, &st) == ERROR_SUCCESS;

        if (!ok) {
            if (slot >= 0) m_Pads[slot] = Pad{};   // disconnected -- release the slot entirely
            continue;
        }
        if (slot < 0) {                            // newly connected: claim a free slot
            for (uint32_t s = 0; s < kMaxGamepads; ++s) {
                if (m_Pads[s].connected) continue;
                m_Pads[s] = Pad{};
                m_Pads[s].xinputUser = (int)user;
                slot = (int)s;
                break;
            }
            if (slot < 0) continue;                // all slots busy
        }

        Pad& pad = m_Pads[slot];
        pad.prevButtons = pad.buttons;
        pad.connected = true;
        pad.buttons = pad.liveButtons = st.Gamepad.wButtons;
        pad.leftTrigger  = st.Gamepad.bLeftTrigger  > XINPUT_GAMEPAD_TRIGGER_THRESHOLD
                         ? st.Gamepad.bLeftTrigger  / 255.0f : 0.0f;
        pad.rightTrigger = st.Gamepad.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD
                         ? st.Gamepad.bRightTrigger / 255.0f : 0.0f;
        NormalizeStick(st.Gamepad.sThumbLX, st.Gamepad.sThumbLY,
                       XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE,  pad.lx, pad.ly);
        NormalizeStick(st.Gamepad.sThumbRX, st.Gamepad.sThumbRY,
                       XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE, pad.rx, pad.ry);
    }
}

// ---- keyboard queries ----
bool InputManager::IsKeyDown(uint8_t vk) const      { return m_KeysThis[vk]; }
bool InputManager::IsKeyPressed(uint8_t vk) const   { return m_KeysThis[vk] && !m_KeysPrev[vk]; }
bool InputManager::IsKeyReleased(uint8_t vk) const  { return !m_KeysThis[vk] && m_KeysPrev[vk]; }

bool InputManager::GetAnyKeyPressed(uint8_t& outKey) const {
    for (int i = 0; i < 256; ++i)
        if (m_KeysThis[i] && !m_KeysPrev[i]) { outKey = (uint8_t)i; return true; }
    return false;
}

std::vector<uint8_t> InputManager::GetKeysPressedThisFrame() const {
    std::vector<uint8_t> out;
    for (int i = 0; i < 256; ++i)
        if (m_KeysThis[i] && !m_KeysPrev[i]) out.push_back((uint8_t)i);
    return out;
}

// ---- mouse queries (all read the SNAPSHOTS, so results are stable for the whole frame) ----
bool InputManager::IsMouseButtonDown(MouseButtons b) const     { return (m_MouseButtonsThis & b) != 0; }
bool InputManager::IsMouseButtonPressed(MouseButtons b) const  { return (m_MouseButtonsThis & b) != 0 && (m_MouseButtonsPrev & b) == 0; }
bool InputManager::IsMouseButtonReleased(MouseButtons b) const { return (m_MouseButtonsThis & b) == 0 && (m_MouseButtonsPrev & b) != 0; }

// ---- gamepad queries (out-of-range / disconnected reads as neutral, never throws) ----
bool InputManager::IsButtonDown(uint32_t i, GamepadButtons b) const {
    return i < kMaxGamepads && m_Pads[i].connected && (m_Pads[i].buttons & b) != 0;
}
bool InputManager::IsButtonPressed(uint32_t i, GamepadButtons b) const {
    return i < kMaxGamepads && m_Pads[i].connected &&
           (m_Pads[i].buttons & b) != 0 && (m_Pads[i].prevButtons & b) == 0;
}
bool InputManager::IsButtonReleased(uint32_t i, GamepadButtons b) const {
    return i < kMaxGamepads && m_Pads[i].connected &&
           (m_Pads[i].buttons & b) == 0 && (m_Pads[i].prevButtons & b) != 0;
}
float InputManager::GetLeftTriggerAxis(uint32_t i) const  { return i < kMaxGamepads ? m_Pads[i].leftTrigger  : 0.0f; }
float InputManager::GetRightTriggerAxis(uint32_t i) const { return i < kMaxGamepads ? m_Pads[i].rightTrigger : 0.0f; }
float InputManager::GetLeftStickX(uint32_t i) const  { return i < kMaxGamepads ? m_Pads[i].lx : 0.0f; }
float InputManager::GetLeftStickY(uint32_t i) const  { return i < kMaxGamepads ? m_Pads[i].ly : 0.0f; }
float InputManager::GetRightStickX(uint32_t i) const { return i < kMaxGamepads ? m_Pads[i].rx : 0.0f; }
float InputManager::GetRightStickY(uint32_t i) const { return i < kMaxGamepads ? m_Pads[i].ry : 0.0f; }
bool InputManager::IsGamepadConnected(uint32_t i) const { return i < kMaxGamepads && m_Pads[i].connected; }

uint32_t InputManager::GetConnectedGamepadCount() const {
    uint32_t n = 0;
    for (uint32_t i = 0; i < kMaxGamepads; ++i) if (m_Pads[i].connected) ++n;
    return n;
}

void InputManager::SetGamepadVibration(uint32_t i, float left, float right) {
    // Rumble is XInput-only: HID pads are read-only here (output reports are per-vendor -- DualSense
    // rumble needs its own protocol, which belongs in a mapping/output layer, not this function).
    // A no-op on a HID pad is correct, not a failure.
    if (i >= kMaxGamepads || !m_Pads[i].connected || !m_XInputSetState) return;
    if (m_Pads[i].xinputUser < 0) return;
    auto clamp01 = [](float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); };
    XINPUT_VIBRATION vib{};
    vib.wLeftMotorSpeed  = (WORD)(clamp01(left)  * 65535.0f);
    vib.wRightMotorSpeed = (WORD)(clamp01(right) * 65535.0f);
    m_XInputSetState((DWORD)m_Pads[i].xinputUser, &vib);
}

}
