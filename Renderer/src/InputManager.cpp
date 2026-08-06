#include "../include/InputManager.h"
#include <algorithm>

InputManager::~InputManager()
{
    if (m_GameInput) m_GameInput->Release();
}

bool InputManager::Initialize()
{
    // GameInputCreate is the inline convenience wrapper (see GameInput.h) around
    // GameInputInitialize(IID_IGameInput, ...) -- returns a real COM reference we own until
    // Release() in the destructor.
    HRESULT hr = GameInputCreate(&m_GameInput);
    return SUCCEEDED(hr);
}

bool InputManager::Contains(const std::vector<uint8_t>& keys, uint8_t virtualKey)
{
    return std::find(keys.begin(), keys.end(), virtualKey) != keys.end();
}

void InputManager::Update()
{
    m_PrevKeysDown = m_KeysDown;
    m_KeysDown.clear();
    m_PrevGamepadButtons = m_GamepadState.buttons;
    m_PrevMouseButtons = m_MouseState.buttons;

    // --- Keyboard ---
    IGameInputReading* keyboardReading = nullptr;
    if (SUCCEEDED(m_GameInput->GetCurrentReading(GameInputKindKeyboard, nullptr, &keyboardReading)))
    {
        uint32_t keyCount = keyboardReading->GetKeyCount();
        if (keyCount > 0)
        {
            std::vector<GameInputKeyState> states(keyCount);
            uint32_t written = keyboardReading->GetKeyState(keyCount, states.data());
            m_KeysDown.reserve(written);
            for (uint32_t i = 0; i < written; ++i)
                m_KeysDown.push_back(states[i].virtualKey);
        }
        keyboardReading->Release();
    }

    // --- Mouse ---
    IGameInputReading* mouseReading = nullptr;
    if (SUCCEEDED(m_GameInput->GetCurrentReading(GameInputKindMouse, nullptr, &mouseReading)))
    {
        mouseReading->GetMouseState(&m_MouseState);
        mouseReading->Release();
    }

    // --- Gamepad --- (first connected gamepad only -- pass a specific IGameInputDevice* instead
    // of nullptr if you need to distinguish multiple controllers later)
    IGameInputReading* gamepadReading = nullptr;
    if (SUCCEEDED(m_GameInput->GetCurrentReading(GameInputKindGamepad, nullptr, &gamepadReading)))
    {
        gamepadReading->GetGamepadState(&m_GamepadState);
        gamepadReading->Release();
    }
    else
    {
        // No gamepad connected this frame -- don't keep stale button/stick state alive.
        m_GamepadState = {};
    }
}

bool InputManager::IsKeyDown(uint8_t virtualKey) const
{
    return Contains(m_KeysDown, virtualKey);
}

bool InputManager::IsKeyPressed(uint8_t virtualKey) const
{
    return Contains(m_KeysDown, virtualKey) && !Contains(m_PrevKeysDown, virtualKey);
}

bool InputManager::IsKeyReleased(uint8_t virtualKey) const
{
    return !Contains(m_KeysDown, virtualKey) && Contains(m_PrevKeysDown, virtualKey);
}

bool InputManager::IsMouseButtonDown(GameInputMouseButtons button) const
{
    return (m_MouseState.buttons & button) != 0;
}

bool InputManager::IsMouseButtonPressed(GameInputMouseButtons button) const
{
    return (m_MouseState.buttons & button) && !(m_PrevMouseButtons & button);
}

bool InputManager::IsMouseButtonReleased(GameInputMouseButtons button) const
{
    return !(m_MouseState.buttons & button) && (m_PrevMouseButtons & button);
}

bool InputManager::IsButtonDown(GameInputGamepadButtons button) const
{
    return (m_GamepadState.buttons & button) != 0;
}

bool InputManager::IsButtonPressed(GameInputGamepadButtons button) const
{
    return (m_GamepadState.buttons & button) && !(m_PrevGamepadButtons & button);
}

bool InputManager::IsButtonReleased(GameInputGamepadButtons button) const
{
    return !(m_GamepadState.buttons & button) && (m_PrevGamepadButtons & button);
}
