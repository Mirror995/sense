#define SDL_MAIN_HANDLED

#include <Windows.h>
#include <SDL3/SDL.h>

#include <array>
#include <initializer_list>

namespace
{
constexpr Uint16 kSonyVendorId = 0x054C;
constexpr ULONGLONG kDoubleClickWindowMs = 300;

SDL_Gamepad* gController = nullptr;
SDL_JoystickID gControllerInstanceId = -1;

bool gSingleClickPending = false;
ULONGLONG gSingleClickDeadline = 0;
ULONGLONG gLastReleaseTime = 0;

void SendChord(const std::initializer_list<WORD> keys)
{
    std::array<INPUT, 16> inputs{};
    const size_t keyCount = keys.size();
    if (keyCount == 0 || keyCount * 2 > inputs.size())
    {
        return;
    }

    size_t index = 0;
    for (WORD key : keys)
    {
        inputs[index].type = INPUT_KEYBOARD;
        inputs[index].ki.wVk = key;
        ++index;
    }
    for (auto it = keys.end(); it != keys.begin();)
    {
        --it;
        inputs[index].type = INPUT_KEYBOARD;
        inputs[index].ki.wVk = *it;
        inputs[index].ki.dwFlags = KEYEVENTF_KEYUP;
        ++index;
    }

    SendInput(static_cast<UINT>(index), inputs.data(), sizeof(INPUT));
}

void TriggerSingleClickAction()
{
    // Win + Alt + PrintScreen
    SendChord({ VK_LWIN, VK_MENU, VK_SNAPSHOT });
}

void TriggerDoubleClickAction()
{
    // Win + G
    SendChord({ VK_LWIN, 'G' });
}

bool IsDualSenseDevice(SDL_JoystickID instanceId)
{
    return SDL_IsGamepad(instanceId) &&
           SDL_GetGamepadVendorForID(instanceId) == kSonyVendorId;
}

void CloseController()
{
    if (gController != nullptr)
    {
        SDL_CloseGamepad(gController);
        gController = nullptr;
        gControllerInstanceId = -1;
    }
}

void TryOpenFirstDualSense()
{
    if (gController != nullptr)
    {
        return;
    }

    int count = 0;
    SDL_JoystickID* gamepads = SDL_GetGamepads(&count);
    if (gamepads == nullptr)
    {
        return;
    }

    for (int i = 0; i < count; ++i)
    {
        const SDL_JoystickID instanceId = gamepads[i];
        if (!IsDualSenseDevice(instanceId))
        {
            continue;
        }

        SDL_Gamepad* opened = SDL_OpenGamepad(instanceId);
        if (opened == nullptr)
        {
            continue;
        }

        gController = opened;
        gControllerInstanceId = SDL_GetGamepadID(gController);
        SDL_free(gamepads);
        return;
    }

    SDL_free(gamepads);
}

bool IsCreateButton(SDL_GamepadButton button)
{
    // 在 SDL 的映射里，DualSense 的 Create 常见为 BACK，部分映射会给到 MISC1。
    return button == SDL_GAMEPAD_BUTTON_BACK ||
           button == SDL_GAMEPAD_BUTTON_MISC1;
}

void HandleCreateButtonUp()
{
    const ULONGLONG now = GetTickCount64();
    if (gSingleClickPending && (now - gLastReleaseTime) <= kDoubleClickWindowMs)
    {
        gSingleClickPending = false;
        TriggerDoubleClickAction();
    }
    else
    {
        gSingleClickPending = true;
        gSingleClickDeadline = now + kDoubleClickWindowMs;
    }

    gLastReleaseTime = now;
}

void FlushPendingSingleClick()
{
    if (!gSingleClickPending)
    {
        return;
    }

    const ULONGLONG now = GetTickCount64();
    if (now >= gSingleClickDeadline)
    {
        gSingleClickPending = false;
        TriggerSingleClickAction();
    }
}
} // namespace

int APIENTRY wWinMain(
    _In_ HINSTANCE /*hInstance*/,
    _In_opt_ HINSTANCE /*hPrevInstance*/,
    _In_ LPWSTR /*lpCmdLine*/,
    _In_ int /*nCmdShow*/)
{
    if (!SDL_Init(SDL_INIT_GAMEPAD | SDL_INIT_EVENTS))
    {
        return 1;
    }

    TryOpenFirstDualSense();

    bool running = true;
    while (running)
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
                running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            switch (event.type)
            {
            case SDL_EVENT_QUIT:
                running = false;
                break;
            case SDL_EVENT_GAMEPAD_ADDED:
                if (gController == nullptr && IsDualSenseDevice(event.gdevice.which))
                {
                    TryOpenFirstDualSense();
                }
                break;
            case SDL_EVENT_GAMEPAD_REMOVED:
                if (event.gdevice.which == gControllerInstanceId)
                {
                    CloseController();
                    gSingleClickPending = false;
                    TryOpenFirstDualSense();
                }
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_UP:
                if (event.gbutton.which == gControllerInstanceId &&
                    IsCreateButton(static_cast<SDL_GamepadButton>(event.gbutton.button)))
                {
                    HandleCreateButtonUp();
                }
                break;
            default:
                break;
            }
        }

        FlushPendingSingleClick();
        SDL_Delay(5);
    }

    CloseController();
    SDL_Quit();
    return 0;
}
