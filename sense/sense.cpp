#define SDL_MAIN_HANDLED

#include <Windows.h>
#include <knownfolders.h>
#include <shellapi.h>
#include <shlobj.h>
#include <SDL3/SDL.h>

#include <array>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <unordered_set>

namespace
{
constexpr Uint16 kSonyVendorId = 0x054C;
constexpr ULONGLONG kDoubleClickWindowMs = 300;
constexpr UINT kTrayCallbackMessage = WM_APP + 1;
constexpr UINT kMenuTogglePauseId = 1001;
constexpr UINT kMenuExitId = 1002;
constexpr UINT kTrayIconId = 1;
constexpr wchar_t kWindowClassName[] = L"sense.tray.window";
constexpr int kDeleteRetryCount = 20;
constexpr DWORD kDeleteRetryDelayMs = 50;

struct CaptureSnapshot
{
    bool valid = false;
    std::filesystem::path directory;
    size_t fileCount = 0;
    std::unordered_set<std::wstring> files;
};

SDL_Gamepad* gController = nullptr;
SDL_JoystickID gControllerInstanceId = -1;
bool gCreateButtonHeld = false;

bool gAwaitingSecondClick = false;
ULONGLONG gLastClickTime = 0;
CaptureSnapshot gFirstClickSnapshot{};

bool gPaused = false;

NOTIFYICONDATAW gTrayIconData{};
bool gTrayIconAdded = false;

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
    SendChord({ VK_LWIN, VK_MENU, VK_SNAPSHOT });
}

void TriggerDoubleClickAction()
{
    SendChord({ VK_LWIN, VK_MENU, 'G' });
}

std::filesystem::path GetCapturesDirectory()
{
    PWSTR videosPath = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Videos, 0, nullptr, &videosPath)))
    {
        return {};
    }

    std::filesystem::path captures(videosPath);
    CoTaskMemFree(videosPath);
    captures /= L"Captures";
    return captures;
}

bool IsScreenshotFile(const std::filesystem::path& path)
{
    const std::wstring ext = path.extension().wstring();
    return _wcsicmp(ext.c_str(), L".png") == 0;
}

CaptureSnapshot TakeCaptureSnapshot()
{
    CaptureSnapshot snapshot{};
    snapshot.directory = GetCapturesDirectory();
    if (snapshot.directory.empty())
    {
        return snapshot;
    }

    std::error_code ec;
    if (!std::filesystem::exists(snapshot.directory, ec) ||
        !std::filesystem::is_directory(snapshot.directory, ec))
    {
        return snapshot;
    }

    for (const auto& entry : std::filesystem::directory_iterator(snapshot.directory, ec))
    {
        if (ec || !entry.is_regular_file(ec))
        {
            continue;
        }
        if (!IsScreenshotFile(entry.path()))
        {
            continue;
        }

        snapshot.files.insert(entry.path().filename().wstring());
    }

    snapshot.fileCount = snapshot.files.size();
    snapshot.valid = true;
    return snapshot;
}

bool DeleteScreenshotFromFirstClick(const CaptureSnapshot& before)
{
    if (!before.valid)
    {
        return false;
    }

    for (int attempt = 0; attempt < kDeleteRetryCount; ++attempt)
    {
        std::error_code ec;
        if (!std::filesystem::exists(before.directory, ec))
        {
            return false;
        }

        std::filesystem::path newestPath;
        std::filesystem::file_time_type newestTime{};
        bool found = false;
        size_t currentCount = 0;

        for (const auto& entry : std::filesystem::directory_iterator(before.directory, ec))
        {
            if (ec || !entry.is_regular_file(ec))
            {
                continue;
            }
            if (!IsScreenshotFile(entry.path()))
            {
                continue;
            }

            ++currentCount;
            const std::wstring name = entry.path().filename().wstring();
            if (before.files.contains(name))
            {
                continue;
            }

            const auto writeTime = entry.last_write_time(ec);
            if (ec)
            {
                continue;
            }
            if (!found || writeTime > newestTime)
            {
                newestTime = writeTime;
                newestPath = entry.path();
                found = true;
            }
        }

        if (found)
        {
            std::error_code removeEc;
            return std::filesystem::remove(newestPath, removeEc) && !removeEc;
        }

        if (currentCount > before.fileCount)
        {
            return false;
        }

        Sleep(kDeleteRetryDelayMs);
    }

    return false;
}

void ResetClickState()
{
    gCreateButtonHeld = false;
    gAwaitingSecondClick = false;
    gLastClickTime = 0;
    gFirstClickSnapshot = {};
}

void HandleCreateButtonDown()
{
    const ULONGLONG now = GetTickCount64();
    if (gAwaitingSecondClick && (now - gLastClickTime) <= kDoubleClickWindowMs)
    {
        gAwaitingSecondClick = false;
        TriggerDoubleClickAction();
        DeleteScreenshotFromFirstClick(gFirstClickSnapshot);
    }
    else
    {
        gFirstClickSnapshot = TakeCaptureSnapshot();
        TriggerSingleClickAction();
        gAwaitingSecondClick = true;
    }

    gLastClickTime = now;
}

void ClearExpiredSecondClickWindow()
{
    if (!gAwaitingSecondClick)
    {
        return;
    }

    const ULONGLONG now = GetTickCount64();
    if ((now - gLastClickTime) > kDoubleClickWindowMs)
    {
        gAwaitingSecondClick = false;
    }
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
    return button == SDL_GAMEPAD_BUTTON_BACK ||
           button == SDL_GAMEPAD_BUTTON_MISC1;
}

void DestroyTrayIcon()
{
    if (gTrayIconAdded)
    {
        Shell_NotifyIconW(NIM_DELETE, &gTrayIconData);
        gTrayIconAdded = false;
    }
}

void ShowTrayMenu(HWND hwnd)
{
    HMENU menu = CreatePopupMenu();
    if (menu == nullptr)
    {
        return;
    }

    AppendMenuW(menu, MF_STRING, kMenuTogglePauseId, gPaused ? L"Resume" : L"Pause");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuExitId, L"Exit");

    POINT cursor{};
    GetCursorPos(&cursor);
    SetForegroundWindow(hwnd);
    const UINT selected = TrackPopupMenu(
        menu,
        TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
        cursor.x,
        cursor.y,
        0,
        hwnd,
        nullptr);

    if (selected == kMenuTogglePauseId)
    {
        gPaused = !gPaused;
        if (gPaused)
        {
            ResetClickState();
        }
    }
    else if (selected == kMenuExitId)
    {
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
    }

    DestroyMenu(menu);
}

LRESULT CALLBACK TrayWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == kTrayCallbackMessage && wParam == kTrayIconId)
    {
        if (lParam == WM_LBUTTONUP || lParam == WM_RBUTTONUP)
        {
            ShowTrayMenu(hwnd);
            return 0;
        }
    }

    switch (message)
    {
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

HWND CreateTrayWindow(HINSTANCE hInstance)
{
    WNDCLASSW wc{};
    wc.lpfnWndProc = TrayWindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = kWindowClassName;
    if (RegisterClassW(&wc) == 0)
    {
        return nullptr;
    }

    return CreateWindowExW(
        0,
        kWindowClassName,
        L"sense",
        WS_OVERLAPPED,
        0,
        0,
        0,
        0,
        nullptr,
        nullptr,
        hInstance,
        nullptr);
}

bool CreateTrayIcon(HWND hwnd)
{
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = kTrayIconId;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    nid.uCallbackMessage = kTrayCallbackMessage;
    nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    lstrcpynW(nid.szTip, L"sense", ARRAYSIZE(nid.szTip));

    if (!Shell_NotifyIconW(NIM_ADD, &nid))
    {
        return false;
    }

    gTrayIconData = nid;
    gTrayIconAdded = true;
    return true;
}
} // namespace

int APIENTRY wWinMain(
    _In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE /*hPrevInstance*/,
    _In_ LPWSTR /*lpCmdLine*/,
    _In_ int /*nCmdShow*/)
{
    HWND trayHwnd = CreateTrayWindow(hInstance);
    if (trayHwnd == nullptr)
    {
        return 1;
    }
    if (!CreateTrayIcon(trayHwnd))
    {
        DestroyWindow(trayHwnd);
        return 1;
    }

    if (!SDL_Init(SDL_INIT_GAMEPAD | SDL_INIT_EVENTS))
    {
        DestroyTrayIcon();
        DestroyWindow(trayHwnd);
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
                    ResetClickState();
                    TryOpenFirstDualSense();
                }
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                if (!gPaused &&
                    event.gbutton.which == gControllerInstanceId &&
                    IsCreateButton(static_cast<SDL_GamepadButton>(event.gbutton.button)) &&
                    !gCreateButtonHeld)
                {
                    gCreateButtonHeld = true;
                    HandleCreateButtonDown();
                }
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_UP:
                if (event.gbutton.which == gControllerInstanceId &&
                    IsCreateButton(static_cast<SDL_GamepadButton>(event.gbutton.button)))
                {
                    gCreateButtonHeld = false;
                }
                break;
            default:
                break;
            }
        }

        if (!gPaused)
        {
            ClearExpiredSecondClickWindow();
        }
        SDL_Delay(5);
    }

    CloseController();
    SDL_Quit();
    DestroyTrayIcon();
    DestroyWindow(trayHwnd);
    return 0;
}
