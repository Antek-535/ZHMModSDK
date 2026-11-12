#include "BetterInstinct.h"

#include <map>

#include <Windows.h>
#include <mmsystem.h>
#include <commdlg.h>
#include <objbase.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <functional>
#include <fstream>
#include <map>
#include <iterator>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include <imgui.h>
#include <wrl/client.h>

#include "Functions.h"
#include "Hooks.h"

#include "Glacier/ZActor.h"
#include "Glacier/ZCameraEntity.h"
#include "Glacier/ZCollision.h"
#include "Glacier/ZEntity.h"
#include "Glacier/EntityFactory.h"
#include "Glacier/ZResource.h"
#include "Glacier/ZEntityManager.h"
#include "Glacier/ZGameLoopManager.h"
#include "Glacier/ZHitman5.h"
#include "Glacier/ZInventory.h"
#include "Glacier/ZObject.h"
#include "Glacier/ZModule.h"
#include "Glacier/ZScene.h"
#include "Glacier/SColorRGB.h"
#include "Glacier/SColorRGBA.h"
#include "Glacier/ZSpatialEntity.h"
#include "Glacier/ZGameTime.h"
#include "Glacier/ZInput.h"

#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "ole32.lib")

static constexpr char kSection[] = "BetterInstinct";
static constexpr float kUnlockLevel = 0.25f; // after running dry, the meter must refill to this before Instinct works again
static constexpr float kMinStart = 0.10f;    // Instinct can't be started below this level
static constexpr float kLowLevel = 0.25f;    // below this the meter turns to the "low" color

// ---------------------------------------------------------------------------
// Instinct glow categories. Each one is a pair of game properties: m_Glow<Name>Color (silhouette / fill) and
// m_Glow<Name>OutlineColor (outline). The default RGB values are the game's own colors.
// ---------------------------------------------------------------------------

struct SGlowCatDef {
    const char* Key;
    const char* Label;
    const char* Tip;
    const char* Names[2]; // property name stems (without Color / OutlineColor); the 2nd may be null
    int FillRgb;
    int OutlineRgb;
};

static constexpr SGlowCatDef kGlowCats[kGlowCatCount] = {
    {"targets", "Targets", "Contract targets (and non-critical targets).", {"m_GlowContractTarget", "m_GlowContractTargetNonCritical"}, 0x520000, 0xCF0000},
    {"special", "Special targets", "Targets the game colors differently (blue).", {"m_GlowContractTargetSpecial", nullptr}, 0x05102B, 0x154EEB},
    {"suspects", "Suspects", "Suspects in Sniper Assassin / suspect-style contracts.", {"m_GlowContractSuspect", nullptr}, 0x3B0AA6, 0x3B0AA6},
    {"civilians", "Civilians", "Non-hostile NPCs.", {"m_GlowCivilian", nullptr}, 0x262626, 0xFFFFFF},
    {"guards", "Guards", "Armed guards and other hostile NPCs.", {"m_GlowEnemies", nullptr}, 0x262626, 0xFFFFFF},
    {"enforcers", "Enforcers", "Enforcers (NPCs who can see through disguises).", {"m_GlowEnforcer", nullptr}, 0x262626, 0xFFFFFF},
    {"witnesses", "Witnesses", "NPCs that have seen you without a mask.", {"m_GlowBackgroundUnmasked", nullptr}, 0xFFEDA3, 0xFFF7D6},
    {"memory", "Witnesses (memory)", "Long-term-memory markers.", {"m_GlowLTMemory", nullptr}, 0x8A5224, 0xFF6C03},
    {"interactables", "Interactables", "Set pieces and other interactive objects.", {"m_GlowSetPiece", nullptr}, 0xFFEDA3, 0xFFF7D6},
    {"climbables", "Climbables", "Ledges, pipes, ladders and other traversal.", {"m_GlowTraversal", nullptr}, 0xFFEDA3, 0xFCF6DE},
    {"items", "Items", "Weapons and other pick-up items.", {"m_GlowGuns", nullptr}, 0xFFEDA3, 0xFFF7D6},
};

static constexpr float kGlowDefFillAlpha = 75.0f / 255.0f;     // typical game values (used until the game's own are read)
static constexpr float kGlowDefOutlineAlpha = 127.0f / 255.0f;

namespace {

// Runs a callable and survives access violations (entities that are half-constructed while a mission loads).
// Kept free of C++ objects with destructors, which is what __try requires.
bool SehInvoke(void (*p_Fn)(void*), void* p_Ctx) {
    __try {
        p_Fn(p_Ctx);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <class F>
bool SafeCall(F& p_Callable) {
    return SehInvoke([](void* p_C) { (*static_cast<F*>(p_C))(); }, &p_Callable);
}

} // namespace

namespace {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// String helpers
// ---------------------------------------------------------------------------

std::wstring Utf8ToWide(const std::string& p_Str) {
    if (p_Str.empty()) return L"";
    const int s_Len = MultiByteToWideChar(CP_UTF8, 0, p_Str.data(), static_cast<int>(p_Str.size()), nullptr, 0);
    std::wstring s_Out(static_cast<size_t>(s_Len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, p_Str.data(), static_cast<int>(p_Str.size()), s_Out.data(), s_Len);
    return s_Out;
}

std::string WideToUtf8(const std::wstring& p_Str) {
    if (p_Str.empty()) return "";
    const int s_Len = WideCharToMultiByte(CP_UTF8, 0, p_Str.data(), static_cast<int>(p_Str.size()), nullptr, 0, nullptr, nullptr);
    std::string s_Out(static_cast<size_t>(s_Len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, p_Str.data(), static_cast<int>(p_Str.size()), s_Out.data(), s_Len, nullptr, nullptr);
    return s_Out;
}

std::wstring FindModuleDir() {
    HMODULE s_Module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&FindModuleDir), &s_Module);
    wchar_t s_Path[MAX_PATH] = {};
    GetModuleFileNameW(s_Module, s_Path, MAX_PATH);
    std::wstring s_Dir = s_Path;
    const size_t s_Slash = s_Dir.find_last_of(L"\\/");
    return s_Dir.substr(0, s_Slash == std::wstring::npos ? 0 : s_Slash + 1);
}

// ---------------------------------------------------------------------------
// Instinct key guard: a low-level keyboard/mouse hook that swallows the Instinct key press while
// Instinct is unusable, and tracks the physical key state (swallowed presses included).
// Best effort: it can only stop input that goes through normal Windows input.
// The game-level block (ZInputAction_Digital detour, further below) is the main blocker.
// ---------------------------------------------------------------------------

std::atomic<bool> g_Block{false};
std::atomic<int> g_Key{0x11};
std::atomic<bool> g_Phys[256];
std::atomic<bool> g_HookOk{false};
std::atomic<bool> g_HookReady{false};
std::atomic<DWORD> g_HookThreadId{0};
DWORD g_Pid = 0;
std::atomic<uint32_t> g_KbEvents{0};
std::atomic<uint32_t> g_MouseEvents{0};
std::atomic<int> g_LastVk{0};
std::atomic<int> g_Wheel{0};          // mouse wheel delta collected by the hook (120 = one notch)
std::atomic<bool> g_ZoomWanted{false}; // zoom is active: the wheel is ours, the game doesn't get it
constexpr ULONG_PTR kOurMarker = 0x464F4341; // tags the key events we inject ourselves, so the hook can ignore them

void SendKey(int p_Vk, bool p_Up);

bool MatchesKey(int p_Configured, int p_Vk) {
    if (p_Configured == p_Vk) return true;
    switch (p_Configured) {
        case VK_SHIFT: return p_Vk == VK_LSHIFT || p_Vk == VK_RSHIFT;
        case VK_CONTROL: return p_Vk == VK_LCONTROL || p_Vk == VK_RCONTROL;
        case VK_MENU: return p_Vk == VK_LMENU || p_Vk == VK_RMENU;
        default: return false;
    }
}

std::vector<int> KeyVariants(int p_Key) {
    switch (p_Key) {
        case VK_SHIFT: return {VK_LSHIFT, VK_RSHIFT};
        case VK_CONTROL: return {VK_LCONTROL, VK_RCONTROL};
        case VK_MENU: return {VK_LMENU, VK_RMENU};
        default: return {p_Key};
    }
}

bool GameIsForeground() {
    DWORD s_Pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &s_Pid);
    return s_Pid == g_Pid;
}

LRESULT CALLBACK KeyboardProc(int p_Code, WPARAM p_W, LPARAM p_L) {
    if (p_Code == HC_ACTION) {
        const auto* s_Kb = reinterpret_cast<const KBDLLHOOKSTRUCT*>(p_L);
        if (s_Kb->dwExtraInfo != kOurMarker) { // everything except what we injected ourselves (remapper/driver keys count)
            const bool s_Down = p_W == WM_KEYDOWN || p_W == WM_SYSKEYDOWN;
            const bool s_Up = p_W == WM_KEYUP || p_W == WM_SYSKEYUP;
            if (s_Down || s_Up) {
                ++g_KbEvents;
                g_LastVk = static_cast<int>(s_Kb->vkCode);
                g_Phys[s_Kb->vkCode & 0xFF] = s_Down;
                if (s_Down && g_Block.load() && MatchesKey(g_Key.load(), static_cast<int>(s_Kb->vkCode)) && GameIsForeground()) {
                    return 1; // swallow the press: the game never sees it
                }
            }
        }
    }
    return CallNextHookEx(nullptr, p_Code, p_W, p_L);
}

LRESULT CALLBACK MouseProc(int p_Code, WPARAM p_W, LPARAM p_L) {
    if (p_Code == HC_ACTION && p_W == WM_MOUSEWHEEL) {
        const auto* s_Wh = reinterpret_cast<const MSLLHOOKSTRUCT*>(p_L);
        if (s_Wh->dwExtraInfo != kOurMarker && g_ZoomWanted.load() && GameIsForeground()) {
            g_Wheel += static_cast<short>(HIWORD(s_Wh->mouseData));
            return 1; // zoom is active: the wheel belongs to the zoom
        }
    }
    if (p_Code == HC_ACTION && p_W != WM_MOUSEMOVE) {
        const auto* s_Ms = reinterpret_cast<const MSLLHOOKSTRUCT*>(p_L);
        if (s_Ms->dwExtraInfo != kOurMarker) {
            int s_Vk = 0;
            bool s_Down = false;
            switch (p_W) {
                case WM_LBUTTONDOWN: s_Vk = VK_LBUTTON; s_Down = true; break;
                case WM_LBUTTONUP: s_Vk = VK_LBUTTON; break;
                case WM_RBUTTONDOWN: s_Vk = VK_RBUTTON; s_Down = true; break;
                case WM_RBUTTONUP: s_Vk = VK_RBUTTON; break;
                case WM_MBUTTONDOWN: s_Vk = VK_MBUTTON; s_Down = true; break;
                case WM_MBUTTONUP: s_Vk = VK_MBUTTON; break;
                case WM_XBUTTONDOWN:
                case WM_XBUTTONUP:
                    s_Vk = HIWORD(s_Ms->mouseData) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2;
                    s_Down = p_W == WM_XBUTTONDOWN;
                    break;
                default: break;
            }

            if (s_Vk != 0) {
                ++g_MouseEvents;
                g_Phys[s_Vk] = s_Down;

                if (s_Down && g_Block.load() && g_Key.load() == s_Vk && GameIsForeground()) {
                    return 1;
                }
            }
        }
    }
    return CallNextHookEx(nullptr, p_Code, p_W, p_L);
}

// Runs on its own thread: installs the hooks and pumps messages until it gets WM_QUIT.
// The thread holds its own reference to this DLL and ends with FreeLibraryAndExitThread, so unloading the mod never
// has to wait for it (waiting for a thread while the mod unloads is what could freeze the game).
DWORD WINAPI HookThreadProc(LPVOID p_Param) {
    HMODULE s_Module = static_cast<HMODULE>(p_Param);

    MSG s_Msg;
    PeekMessageW(&s_Msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE); // make sure this thread has a message queue
    const DWORD s_MyId = GetCurrentThreadId();
    g_HookThreadId = s_MyId;

    HHOOK s_Kb = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardProc, s_Module, 0);
    HHOOK s_Ms = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, s_Module, 0);
    g_HookOk = s_Kb != nullptr && s_Ms != nullptr;
    g_HookReady = true;

    while (GetMessageW(&s_Msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&s_Msg);
        DispatchMessageW(&s_Msg);
    }

    if (s_Kb) UnhookWindowsHookEx(s_Kb);
    if (s_Ms) UnhookWindowsHookEx(s_Ms);

    if (g_HookThreadId.load() == s_MyId) {
        g_HookOk = false;
        g_HookReady = false;
        g_HookThreadId = 0;
    }

    FreeLibraryAndExitThread(s_Module, 0); // drops our DLL reference and ends the thread without returning into DLL code
    return 0;
}

class InputGuard {
public:
    InputGuard() {
        g_Pid = GetCurrentProcessId();
        for (auto& s_Key : g_Phys) s_Key = false;

        // A hook thread from a previous load of the mod may still be finishing: give it a moment.
        for (int i = 0; i < 100 && g_HookThreadId.load() != 0; ++i) Sleep(5);
        g_HookReady = false;
        g_HookOk = false;

        HMODULE s_Module = nullptr; // (no UNCHANGED_REFCOUNT flag: this takes a reference for the thread)
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&HookThreadProc), &s_Module) ||
            s_Module == nullptr) {
            return;
        }

        HANDLE s_Thread = CreateThread(nullptr, 0, HookThreadProc, s_Module, 0, nullptr);
        if (s_Thread == nullptr) {
            FreeLibrary(s_Module);
            return;
        }
        CloseHandle(s_Thread);

        for (int i = 0; i < 200 && !g_HookReady.load(); ++i) Sleep(5); // wait (max ~1s) for the hooks to be installed
    }

    ~InputGuard() {
        // Only ask the thread to stop; it unhooks and exits on its own. Never wait for it here.
        g_Block = false;
        const DWORD s_Id = g_HookThreadId.load();
        if (g_HookReady.load() && s_Id != 0) {
            PostThreadMessageW(s_Id, WM_QUIT, 0, 0);
        }
    }
};

std::unique_ptr<InputGuard> g_Guard;

bool AsyncKeyDown(int p_Key) {
    for (int s_Vk : KeyVariants(p_Key)) {
        if ((GetAsyncKeyState(s_Vk) & 0x8000) != 0) return true;
    }
    return false;
}

bool HookKeyDown(int p_Key) {
    if (!g_HookOk.load()) return false;
    for (int s_Vk : KeyVariants(p_Key)) {
        if (g_Phys[s_Vk & 0xFF].load()) return true;
    }
    return false;
}

// Down if either Windows' own key state or the hook says so (the hook also sees keys we swallowed).
bool PhysKeyDown(int p_Key) {
    return AsyncKeyDown(p_Key) || HookKeyDown(p_Key);
}

void SendKey(int p_Vk, bool p_Up) {
    INPUT s_In = {};
    switch (p_Vk) {
        case VK_LBUTTON: case VK_RBUTTON: case VK_MBUTTON: case VK_XBUTTON1: case VK_XBUTTON2:
            s_In.type = INPUT_MOUSE;
            if (p_Vk == VK_LBUTTON) s_In.mi.dwFlags = p_Up ? MOUSEEVENTF_LEFTUP : MOUSEEVENTF_LEFTDOWN;
            else if (p_Vk == VK_RBUTTON) s_In.mi.dwFlags = p_Up ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_RIGHTDOWN;
            else if (p_Vk == VK_MBUTTON) s_In.mi.dwFlags = p_Up ? MOUSEEVENTF_MIDDLEUP : MOUSEEVENTF_MIDDLEDOWN;
            else {
                s_In.mi.dwFlags = p_Up ? MOUSEEVENTF_XUP : MOUSEEVENTF_XDOWN;
                s_In.mi.mouseData = p_Vk == VK_XBUTTON1 ? XBUTTON1 : XBUTTON2;
            }
            break;
        default: {
            s_In.type = INPUT_KEYBOARD;
            s_In.ki.wVk = static_cast<WORD>(p_Vk);
            s_In.ki.wScan = static_cast<WORD>(MapVirtualKeyW(static_cast<UINT>(p_Vk), MAPVK_VK_TO_VSC));
            DWORD s_Flags = p_Up ? KEYEVENTF_KEYUP : 0;
            switch (p_Vk) { // extended keys
                case VK_RCONTROL: case VK_RMENU: case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
                case VK_PRIOR: case VK_NEXT: case VK_END: case VK_HOME: case VK_INSERT: case VK_DELETE:
                    s_Flags |= KEYEVENTF_EXTENDEDKEY;
                    break;
                default: break;
            }
            s_In.ki.dwFlags = s_Flags;
            break;
        }
    }
    if (s_In.type == INPUT_MOUSE) s_In.mi.dwExtraInfo = kOurMarker;
    else s_In.ki.dwExtraInfo = kOurMarker;
    SendInput(1, &s_In, sizeof(INPUT));
}

// ---------------------------------------------------------------------------
// "Browse..." file dialog for the custom Instinct sound. It runs on its own thread, so the game keeps rendering
// while the dialog is open. Like the input-hook thread it holds its own reference to this DLL.
// ---------------------------------------------------------------------------

std::atomic<bool> g_BrowseBusy{false};   // a dialog is currently open
std::atomic<bool> g_BrowseReady{false};  // the player picked a file (g_BrowseResult)
std::mutex g_BrowseMutex;
std::wstring g_BrowseResult;
std::wstring g_BrowseStartDir;
HWND g_BrowseOwner = nullptr;

DWORD WINAPI BrowseThreadProc(LPVOID p_Param) {
    HMODULE s_Module = static_cast<HMODULE>(p_Param);
    const HRESULT s_Com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    std::wstring s_Dir;
    {
        std::lock_guard<std::mutex> s_Lock(g_BrowseMutex);
        s_Dir = g_BrowseStartDir;
    }

    wchar_t s_File[MAX_PATH * 2] = {};
    OPENFILENAMEW s_Ofn = {};
    s_Ofn.lStructSize = sizeof(s_Ofn);
    s_Ofn.hwndOwner = g_BrowseOwner;
    s_Ofn.lpstrFilter = L"Sound files (*.wav;*.mp3)\0*.wav;*.mp3\0All files (*.*)\0*.*\0\0";
    s_Ofn.nFilterIndex = 1;
    s_Ofn.lpstrFile = s_File;
    s_Ofn.nMaxFile = static_cast<DWORD>(sizeof(s_File) / sizeof(s_File[0]));
    s_Ofn.lpstrInitialDir = s_Dir.empty() ? nullptr : s_Dir.c_str();
    s_Ofn.lpstrTitle = L"Choose an Instinct sound";
    s_Ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;

    if (GetOpenFileNameW(&s_Ofn)) {
        std::lock_guard<std::mutex> s_Lock(g_BrowseMutex);
        g_BrowseResult = s_File;
        g_BrowseReady = true;
    }

    if (SUCCEEDED(s_Com)) CoUninitialize();
    g_BrowseBusy = false;
    FreeLibraryAndExitThread(s_Module, 0);
    return 0;
}

void StartBrowse(const std::wstring& p_StartDir) {
    if (g_BrowseBusy.exchange(true)) return; // already open

    {
        std::lock_guard<std::mutex> s_Lock(g_BrowseMutex);
        g_BrowseStartDir = p_StartDir;
    }
    g_BrowseOwner = GetForegroundWindow(); // keeps the dialog on top of the game window

    HMODULE s_Module = nullptr; // (no UNCHANGED_REFCOUNT flag: this takes a reference for the thread)
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&BrowseThreadProc), &s_Module);
    HANDLE s_Thread = s_Module ? CreateThread(nullptr, 0, BrowseThreadProc, s_Module, 0, nullptr) : nullptr;
    if (s_Thread) {
        CloseHandle(s_Thread);
    } else {
        if (s_Module) FreeLibrary(s_Module);
        g_BrowseBusy = false;
    }
}

// ---------------------------------------------------------------------------
// Built-in sound presets (written to the sound folder on first run, if missing).
// ---------------------------------------------------------------------------

constexpr uint32_t kRate = 22050;
constexpr float kPi = 3.14159265358979f;
using Samples = std::vector<float>;

bool WriteWav(const fs::path& p_Path, const Samples& p_Samples, float p_Gain) {
    const uint32_t n = static_cast<uint32_t>(p_Samples.size());
    const uint32_t s_Bytes = n * 2;
    std::vector<uint8_t> s_Wav(44 + s_Bytes);
    auto* p = s_Wav.data();
    auto w32 = [&](size_t o, uint32_t v) { std::memcpy(p + o, &v, 4); };
    auto w16 = [&](size_t o, uint16_t v) { std::memcpy(p + o, &v, 2); };

    std::memcpy(p + 0, "RIFF", 4);
    w32(4, 36 + s_Bytes);
    std::memcpy(p + 8, "WAVEfmt ", 8);
    w32(16, 16);
    w16(20, 1);
    w16(22, 1);
    w32(24, kRate);
    w32(28, kRate * 2);
    w16(32, 2);
    w16(34, 16);
    std::memcpy(p + 36, "data", 4);
    w32(40, s_Bytes);

    for (uint32_t i = 0; i < n; ++i) {
        const int16_t s_Pcm = static_cast<int16_t>(std::clamp(p_Samples[i] * p_Gain, -1.0f, 1.0f) * 32767.0f);
        std::memcpy(p + 44 + i * 2, &s_Pcm, 2);
    }

    std::ofstream s_File(p_Path, std::ios::binary);
    if (!s_File) return false;
    s_File.write(reinterpret_cast<const char*>(s_Wav.data()), static_cast<std::streamsize>(s_Wav.size()));
    return true;
}

Samples PresetLowHum() { // 2.0 s, loops seamlessly (whole number of cycles)
    const uint32_t n = kRate * 2;
    Samples s(n);
    for (uint32_t i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / kRate;
        const float v = 0.5f * std::sin(2 * kPi * 55.0f * t) + 0.3f * std::sin(2 * kPi * 82.5f * t) + 0.2f * std::sin(2 * kPi * 110.0f * t);
        s[i] = v * (0.75f + 0.25f * std::sin(2 * kPi * 0.5f * t));
    }
    return s;
}

Samples PresetHeartbeat() { // 1.0 s loop
    const uint32_t n = kRate;
    Samples s(n, 0.0f);
    auto thump = [&](float p_Start, float p_Amp) {
        for (uint32_t i = 0; i < n; ++i) {
            const float t = static_cast<float>(i) / kRate - p_Start;
            if (t < 0.0f) continue;
            const float env = std::exp(-t * 16.0f) * std::min(1.0f, t / 0.004f);
            s[i] += p_Amp * env * (std::sin(2 * kPi * 55.0f * t) + 0.5f * std::sin(2 * kPi * 110.0f * t));
        }
    };
    thump(0.0f, 1.0f);
    thump(0.22f, 0.75f);
    return s;
}

Samples PresetPulseSweep() { // 1.5 s loop, fades to silence at both ends
    const uint32_t n = static_cast<uint32_t>(kRate * 1.5f);
    Samples s(n);
    double s_Phase = 0.0;
    for (uint32_t i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / n;
        s_Phase += 2.0 * kPi * (220.0f + 380.0f * t) / kRate;
        const float env = std::sin(kPi * t);
        s[i] = static_cast<float>(std::sin(s_Phase)) * env * env * 0.8f;
    }
    return s;
}

// Case-insensitive "does p_Hay contain p_NeedleLower" (p_NeedleLower must already be lowercase). No allocations.
bool ContainsNoCase(const char* p_Hay, const std::string& p_NeedleLower) {
    if (p_NeedleLower.empty()) return false;
    const size_t s_HayLen = std::strlen(p_Hay);
    const size_t s_NeedleLen = p_NeedleLower.size();
    if (s_NeedleLen > s_HayLen) return false;
    for (size_t i = 0; i + s_NeedleLen <= s_HayLen; ++i) {
        size_t j = 0;
        while (j < s_NeedleLen && std::tolower(static_cast<unsigned char>(p_Hay[i + j])) == static_cast<unsigned char>(p_NeedleLower[j])) ++j;
        if (j == s_NeedleLen) return true;
    }
    return false;
}

// Stable, always-writable place for our own settings file (independent of where the DLL is loaded from).
std::wstring ConfigDir() {
    wchar_t s_Buf[MAX_PATH] = {};
    const DWORD s_Len = GetEnvironmentVariableW(L"APPDATA", s_Buf, MAX_PATH);
    if (s_Len == 0 || s_Len >= MAX_PATH) return L"";
    return std::wstring(s_Buf) + L"\\BetterInstinct\\";
}

std::string OneLine(std::string p_Str) { // values are stored one per line
    p_Str.erase(std::remove(p_Str.begin(), p_Str.end(), '\r'), p_Str.end());
    p_Str.erase(std::remove(p_Str.begin(), p_Str.end(), '\n'), p_Str.end());
    return p_Str;
}

} // namespace

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

BetterInstinct::~BetterInstinct() {
    if (m_ConfigReady) {
        SaveConfigFile();
    }

    m_InputBlock = false;
    m_CaptureNames = false;

    const ZMemberDelegate<BetterInstinct, void(const SGameUpdateEvent&)> s_Delegate(this, &BetterInstinct::OnFrameUpdate);
    Globals::GameLoopManager->UnregisterFrameUpdate(s_Delegate, 1, EUpdateMode::eUpdatePlayMode);

    if (m_HasPlayer.load()) RestoreGlowColors(); // put the game's own colors back
    RestoreTint();
    CloseSound();
    g_Guard.reset(); // removes the input hooks

    if (m_Touched && Globals::GameTimeManager) {
        Globals::GameTimeManager->m_fGameTimeMultiplier = 1.0f;
    }
}

void BetterInstinct::Init() {
    // Lets us answer "not pressed" to the game when it asks about the Instinct action.
    Hooks::ZInputAction_Digital->AddDetour(this, &BetterInstinct::ZInputAction_Digital);
    // Catches the Instinct glow config entity when the game activates it.
    Hooks::ZEntityManager_ActivateEntity->AddDetour(this, &BetterInstinct::ZEntityManager_ActivateEntity);
    // Main way to find the glow config entity: the game builds it from its template, and we look at the result right after.
    Hooks::ZTemplateEntityFactory_ConfigureEntity->AddDetour(this, &BetterInstinct::ZTemplateEntityFactory_ConfigureEntity);
}

void BetterInstinct::OnEngineInitialized() {
    InitGlowDefaults();
    LoadSettings();
    RebuildPatterns(); // default patterns (LoadSoundText below may replace them)

    // Register the game-loop update first, so nothing below can stop the core feature from running.
    const ZMemberDelegate<BetterInstinct, void(const SGameUpdateEvent&)> s_Delegate(this, &BetterInstinct::OnFrameUpdate);
    Globals::GameLoopManager->RegisterFrameUpdate(s_Delegate, 1, EUpdateMode::eUpdatePlayMode);

    try {
        m_ModDir = FindModuleDir();
        m_SoundDir = m_ModDir + L"InstinctSounds\\";
        std::error_code s_Ec;
        fs::create_directories(fs::path(m_SoundDir), s_Ec);
        EnsurePresets();
        LoadSoundText();
        ScanSounds();
    } catch (...) {
        m_SoundStatus = "Couldn't prepare the sound folder.";
    }

    // Our own settings file wins over everything loaded above (first run: it gets created from the current values).
    bool s_ConfigFound = false;
    try {
        s_ConfigFound = LoadConfigFile();
        if (s_ConfigFound) ScanSounds(); // re-select the saved sound
    } catch (...) {
    }
    SaveConfigFile(); // also rewrites an older config file with every current option
    m_ConfigReady = true;

    try {
        g_Key = m_InstinctKey;
        g_Guard = std::make_unique<InputGuard>();
    } catch (...) {
        g_HookOk = false;
    }
}

// ---------------------------------------------------------------------------
// Game-level input blocking
// ---------------------------------------------------------------------------

void BetterInstinct::RebuildPatterns() {
    std::vector<std::string> s_New;
    std::string s_Cur;

    auto s_Flush = [&]() {
        const size_t s_A = s_Cur.find_first_not_of(" \t");
        if (s_A != std::string::npos) {
            const size_t s_B = s_Cur.find_last_not_of(" \t");
            std::string s_Item = s_Cur.substr(s_A, s_B - s_A + 1);
            std::transform(s_Item.begin(), s_Item.end(), s_Item.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            s_New.push_back(std::move(s_Item));
        }
        s_Cur.clear();
    };

    for (const char* p = m_ActionPatterns; *p; ++p) {
        if (*p == ',') s_Flush();
        else s_Cur += *p;
    }
    s_Flush();

    std::lock_guard<std::mutex> s_Lock(m_NameMutex);
    m_Patterns = std::move(s_New);
}

bool BetterInstinct::ActionMatches(const char* p_Name) {
    std::lock_guard<std::mutex> s_Lock(m_NameMutex);
    for (const auto& s_Pattern : m_Patterns) {
        if (ContainsNoCase(p_Name, s_Pattern)) return true;
    }
    return false;
}

// Called for every entity the game activates. Cheap: the answer is cached per entity type.
DEFINE_PLUGIN_DETOUR(BetterInstinct, void, ZEntityManager_ActivateEntity, ZEntityManager* th, ZEntityRef* entity, void* a3) {
    CatchGlowEntity(entity);
    ApplyGlowOnActivate(entity); // runs before the game's own activation code reads the colors
    return HookResult<void>(HookAction::Continue());
}

void BetterInstinct::CatchGlowEntity(const ZEntityRef* p_Ref) {
    if (!p_Ref) return;

    // Anything that can fault (reading the entity) runs outside the lock: after an access violation the lock's
    // destructor would not run and the mutex would stay locked forever.
    ZEntityImpl* s_Entity = nullptr;
    ZEntityType* s_Type = nullptr;
    auto s_ReadType = [&]() {
        s_Entity = p_Ref->GetEntity();
        s_Type = s_Entity ? s_Entity->GetType() : nullptr;
        if (s_Type && !s_Type->m_pPropertyData) s_Type = nullptr;
    };
    if (!SafeCall(s_ReadType) || !s_Type) return;

    std::vector<uint32_t> s_Anchors;
    int s_Known = -1; // -1 = type not seen yet, 0 = no, 1 = yes
    {
        std::lock_guard<std::mutex> s_Lock(m_CatchMutex);
        if (m_CatchAnchors.empty()) {
            for (int i = 0; i < kGlowCatCount; ++i) {
                for (const char* s_Stem : kGlowCats[i].Names) {
                    if (!s_Stem) continue;
                    if (i == 0 || i == 3 || i == 4 || i == 10) {
                        m_CatchAnchors.push_back(Hash::Crc32((std::string(s_Stem) + "Color").c_str()));
                    }
                }
            }
        }
        const auto s_Cached = m_CatchTypeCache.find(s_Type);
        if (s_Cached != m_CatchTypeCache.end()) s_Known = s_Cached->second ? 1 : 0;
        else s_Anchors = m_CatchAnchors;
    }

    if (s_Known < 0) {
        bool s_Has = false;
        auto s_Check = [&]() {
            for (const uint32_t s_Id : s_Anchors) {
                if (s_Type->FindProperty(s_Id)) { s_Has = true; break; }
            }
        };
        SafeCall(s_Check);
        s_Known = s_Has ? 1 : 0;
        std::lock_guard<std::mutex> s_Lock(m_CatchMutex);
        m_CatchTypeCache[s_Type] = s_Has;
    }
    if (s_Known != 1) return;

    {
        std::lock_guard<std::mutex> s_Lock(m_CatchMutex);
        for (const ZEntityRef& s_Old : m_GlowCaught) {
            if (s_Old.m_pObj == p_Ref->m_pObj) return;
        }
        if (m_GlowCaught.size() >= 16) m_GlowCaught.erase(m_GlowCaught.begin()); // only the newest few matter
        m_GlowCaught.push_back(*p_Ref);
    }
    m_GlowRescanRequest.store(true);
}

// The game polls this for every input action, many times per frame, possibly from several threads.
DEFINE_PLUGIN_DETOUR(BetterInstinct, bool, ZInputAction_Digital, ZInputAction* th, int32_t controllerId) {
    const bool s_Block = m_InputBlock.load();
    const bool s_Capture = m_CaptureNames.load();

    if (!th || (!s_Block && !s_Capture)) {
        return HookResult<bool>(HookAction::Continue());
    }

    const char* s_Name = th->m_szName;
    if (!s_Name) {
        return HookResult<bool>(HookAction::Continue());
    }

    if (s_Capture) {
        std::lock_guard<std::mutex> s_Lock(m_NameMutex);
        if (m_SeenNames.size() < 1000) {
            m_SeenNames.insert(s_Name);
        }
    }

    if (s_Block && ActionMatches(s_Name)) {
        ++m_BlockedPolls;
        return HookResult(HookAction::Return(), false);
    }

    return HookResult<bool>(HookAction::Continue());
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

void BetterInstinct::LoadSettings() {
    m_InstinctKey = static_cast<int>(GetSettingInt(kSection, "instinct_key", m_InstinctKey));
    m_ToggleMode = GetSettingBool(kSection, "toggle_mode", m_ToggleMode);
    m_BlockEnabled = GetSettingBool(kSection, "block_enabled", m_BlockEnabled);
    m_FocusScale = std::clamp(static_cast<float>(GetSettingDouble(kSection, "focus_scale", m_FocusScale)), 0.05f, 0.95f);
    m_BlendSpeed = std::clamp(static_cast<float>(GetSettingDouble(kSection, "blend_speed", m_BlendSpeed)), 1.0f, 30.0f);
    m_DrainSeconds = std::clamp(static_cast<float>(GetSettingDouble(kSection, "drain_seconds", m_DrainSeconds)), 2.0f, 120.0f);
    m_RechargeSeconds = std::clamp(static_cast<float>(GetSettingDouble(kSection, "recharge_seconds", m_RechargeSeconds)), 2.0f, 300.0f);
    m_RechargeDelay = std::clamp(static_cast<float>(GetSettingDouble(kSection, "recharge_delay", m_RechargeDelay)), 0.0f, 10.0f);

    m_HudEnabled = GetSettingBool(kSection, "hud_enabled", m_HudEnabled);
    m_HudAlways = GetSettingBool(kSection, "hud_always", m_HudAlways);
    m_HudWidth = std::clamp(static_cast<float>(GetSettingDouble(kSection, "hud_width", m_HudWidth)), 0.05f, 0.5f);
    m_HudYOffset = std::clamp(static_cast<float>(GetSettingDouble(kSection, "hud_y", m_HudYOffset)), 0.0f, 1080.0f);
    m_HudX = std::clamp(static_cast<float>(GetSettingDouble(kSection, "hud_x", m_HudX)), 0.0f, 1.0f);
    m_HudVertical = GetSettingBool(kSection, "hud_vertical", m_HudVertical);
    m_FireFx = GetSettingBool(kSection, "fire_fx", m_FireFx);
    m_FireAmount = std::clamp(static_cast<float>(GetSettingDouble(kSection, "fire_amount", m_FireAmount)), 0.2f, 2.5f);
    m_HudScale = std::clamp(static_cast<float>(GetSettingDouble(kSection, "hud_scale", m_HudScale)), 0.5f, 3.0f);
    m_HudOpacity = std::clamp(static_cast<float>(GetSettingDouble(kSection, "hud_opacity", m_HudOpacity)), 0.1f, 1.0f);
    m_FillColor[0] = std::clamp(static_cast<float>(GetSettingDouble(kSection, "fill_r", m_FillColor[0])), 0.0f, 1.0f);
    m_FillColor[1] = std::clamp(static_cast<float>(GetSettingDouble(kSection, "fill_g", m_FillColor[1])), 0.0f, 1.0f);
    m_FillColor[2] = std::clamp(static_cast<float>(GetSettingDouble(kSection, "fill_b", m_FillColor[2])), 0.0f, 1.0f);
    m_LowColor[0] = std::clamp(static_cast<float>(GetSettingDouble(kSection, "low_r", m_LowColor[0])), 0.0f, 1.0f);
    m_LowColor[1] = std::clamp(static_cast<float>(GetSettingDouble(kSection, "low_g", m_LowColor[1])), 0.0f, 1.0f);
    m_LowColor[2] = std::clamp(static_cast<float>(GetSettingDouble(kSection, "low_b", m_LowColor[2])), 0.0f, 1.0f);

    m_TintEnabled = GetSettingBool(kSection, "tint_enabled", m_TintEnabled);
    m_TintRemove = GetSettingBool(kSection, "tint_remove", m_TintRemove);
    m_InstinctTint[0] = std::clamp(static_cast<float>(GetSettingDouble(kSection, "tint_r", m_InstinctTint[0])), 0.0f, 1.0f);
    m_InstinctTint[1] = std::clamp(static_cast<float>(GetSettingDouble(kSection, "tint_g", m_InstinctTint[1])), 0.0f, 1.0f);
    m_InstinctTint[2] = std::clamp(static_cast<float>(GetSettingDouble(kSection, "tint_b", m_InstinctTint[2])), 0.0f, 1.0f);
    m_TintStrength = std::clamp(static_cast<float>(GetSettingDouble(kSection, "lut_strength", m_TintStrength)), 0.0f, 1.0f);
    m_TintKeepBrightness = GetSettingBool(kSection, "tint_keep_brightness", m_TintKeepBrightness);
    m_TintBrightness = std::clamp(static_cast<float>(GetSettingDouble(kSection, "lut_brightness", m_TintBrightness)), 0.25f, 3.0f);

    // Older versions saved a plain white meter colour: switch to the new gold look once.
    if (GetSettingInt(kSection, "hud_style", 0) < 2) {
        m_FillColor[0] = 1.00f;
        m_FillColor[1] = 0.78f;
        m_FillColor[2] = 0.28f;
    }

    m_SoundEnabled = GetSettingBool(kSection, "sound_enabled", m_SoundEnabled);
    m_SoundSource = std::clamp(static_cast<int>(GetSettingInt(kSection, "sound_source", m_SoundSource)), 0, 1);
    m_SoundVolume = std::clamp(static_cast<float>(GetSettingDouble(kSection, "sound_volume", m_SoundVolume)), 0.0f, 1.0f);
    m_FadeOutOn = GetSettingBool(kSection, "fade_out_on", m_FadeOutOn);
    m_FadeOutMs = std::clamp(static_cast<float>(GetSettingDouble(kSection, "fade_out_ms", m_FadeOutMs)), 0.0f, 3000.0f);
    m_FadeInOn = GetSettingBool(kSection, "fade_in_on", m_FadeInOn);
    m_FadeInMs = std::clamp(static_cast<float>(GetSettingDouble(kSection, "fade_in_ms", m_FadeInMs)), 0.0f, 3000.0f);
}

void BetterInstinct::SaveSettings() {
    SetSettingInt(kSection, "instinct_key", m_InstinctKey);
    SetSettingInt(kSection, "hud_style", 2);
    SetSettingBool(kSection, "toggle_mode", m_ToggleMode);
    SetSettingBool(kSection, "block_enabled", m_BlockEnabled);
    SetSettingDouble(kSection, "focus_scale", m_FocusScale);
    SetSettingDouble(kSection, "blend_speed", m_BlendSpeed);
    SetSettingDouble(kSection, "drain_seconds", m_DrainSeconds);
    SetSettingDouble(kSection, "recharge_seconds", m_RechargeSeconds);
    SetSettingDouble(kSection, "recharge_delay", m_RechargeDelay);

    SetSettingBool(kSection, "hud_enabled", m_HudEnabled);
    SetSettingBool(kSection, "hud_always", m_HudAlways);
    SetSettingDouble(kSection, "hud_width", m_HudWidth);
    SetSettingDouble(kSection, "hud_y", m_HudYOffset);
    SetSettingDouble(kSection, "hud_x", m_HudX);
    SetSettingBool(kSection, "hud_vertical", m_HudVertical);
    SetSettingBool(kSection, "fire_fx", m_FireFx);
    SetSettingDouble(kSection, "fire_amount", m_FireAmount);
    SetSettingDouble(kSection, "hud_scale", m_HudScale);
    SetSettingDouble(kSection, "hud_opacity", m_HudOpacity);
    SetSettingDouble(kSection, "fill_r", m_FillColor[0]);
    SetSettingDouble(kSection, "fill_g", m_FillColor[1]);
    SetSettingDouble(kSection, "fill_b", m_FillColor[2]);
    SetSettingDouble(kSection, "low_r", m_LowColor[0]);
    SetSettingDouble(kSection, "low_g", m_LowColor[1]);
    SetSettingDouble(kSection, "low_b", m_LowColor[2]);

    SetSettingBool(kSection, "tint_enabled", m_TintEnabled);
    SetSettingBool(kSection, "tint_remove", m_TintRemove);
    SetSettingDouble(kSection, "tint_r", m_InstinctTint[0]);
    SetSettingDouble(kSection, "tint_g", m_InstinctTint[1]);
    SetSettingDouble(kSection, "tint_b", m_InstinctTint[2]);
    SetSettingDouble(kSection, "lut_strength", m_TintStrength);
    SetSettingBool(kSection, "tint_keep_brightness", m_TintKeepBrightness);
    SetSettingDouble(kSection, "lut_brightness", m_TintBrightness);

    SetSettingBool(kSection, "sound_enabled", m_SoundEnabled);
    SetSettingInt(kSection, "sound_source", m_SoundSource);
    SetSettingDouble(kSection, "sound_volume", m_SoundVolume);
    SetSettingBool(kSection, "fade_out_on", m_FadeOutOn);
    SetSettingDouble(kSection, "fade_out_ms", m_FadeOutMs);
    SetSettingBool(kSection, "fade_in_on", m_FadeInOn);
    SetSettingDouble(kSection, "fade_in_ms", m_FadeInMs);

    SaveSoundText();
    SaveConfigFile();
}

// The text values (selected file name, custom path, Instinct action name(s)) live in a small text file next to the DLL.
void BetterInstinct::LoadSoundText() {
    std::ifstream s_File(fs::path(m_ModDir + L"BetterInstinct.sound.txt"));
    std::string s_Line;
    if (std::getline(s_File, s_Line)) {
        while (!s_Line.empty() && (s_Line.back() == '\r' || s_Line.back() == '\n')) s_Line.pop_back();
        m_PresetName = s_Line;
    }
    if (std::getline(s_File, s_Line)) {
        while (!s_Line.empty() && (s_Line.back() == '\r' || s_Line.back() == '\n')) s_Line.pop_back();
        std::snprintf(m_CustomPath, sizeof(m_CustomPath), "%s", s_Line.c_str());
    }
    if (std::getline(s_File, s_Line)) {
        while (!s_Line.empty() && (s_Line.back() == '\r' || s_Line.back() == '\n')) s_Line.pop_back();
        if (!s_Line.empty()) {
            std::snprintf(m_ActionPatterns, sizeof(m_ActionPatterns), "%s", s_Line.c_str());
        }
    }
    RebuildPatterns();
}

void BetterInstinct::SaveSoundText() {
    std::ofstream s_File(fs::path(m_ModDir + L"BetterInstinct.sound.txt"), std::ios::trunc);
    if (s_File) {
        s_File << m_PresetName << "\n" << m_CustomPath << "\n" << m_ActionPatterns << "\n";
    }
}

bool BetterInstinct::SaveConfigFile() {
    std::wstring s_Dir = ConfigDir();
    if (s_Dir.empty()) s_Dir = m_ModDir;
    if (s_Dir.empty()) return false;

    std::error_code s_Ec;
    fs::create_directories(fs::path(s_Dir), s_Ec);

    const fs::path s_Final = fs::path(s_Dir) / L"BetterInstinct.cfg";
    const fs::path s_Tmp = fs::path(s_Dir) / L"BetterInstinct.cfg.tmp";

    {
        std::ofstream s_File(s_Tmp, std::ios::trunc);
        if (!s_File) return false;

        auto s_Int = [&](const char* k, long long v) { s_File << k << "=" << v << "\n"; };
        auto s_Flt = [&](const char* k, float v) { s_File << k << "=" << std::to_string(v) << "\n"; };
        auto s_Bool = [&](const char* k, bool v) { s_File << k << "=" << (v ? 1 : 0) << "\n"; };
        auto s_Str = [&](const char* k, const std::string& v) { s_File << k << "=" << OneLine(v) << "\n"; };

        s_Int("instinct_key", m_InstinctKey);
        s_Bool("toggle_mode", m_ToggleMode);
        s_Bool("block_enabled", m_BlockEnabled);
        s_Flt("focus_scale", m_FocusScale);
        s_Flt("blend_speed", m_BlendSpeed);
        s_Flt("drain_seconds", m_DrainSeconds);
        s_Flt("recharge_seconds", m_RechargeSeconds);
        s_Flt("recharge_delay", m_RechargeDelay);

        s_Bool("hud_enabled", m_HudEnabled);
        s_Bool("hud_always", m_HudAlways);
        s_Flt("hud_width", m_HudWidth);
        s_Flt("hud_y", m_HudYOffset);
        s_Flt("hud_x", m_HudX);
        s_Bool("hud_vertical", m_HudVertical);
        s_Bool("fire_fx", m_FireFx);
        s_Flt("fire_amount", m_FireAmount);
        s_Flt("hud_scale", m_HudScale);
        s_Flt("hud_opacity", m_HudOpacity);
        s_Flt("bar_bottom_r", m_BarBottom[0]);
        s_Flt("bar_bottom_g", m_BarBottom[1]);
        s_Flt("bar_bottom_b", m_BarBottom[2]);
        s_Flt("bar_top_r", m_BarTop[0]);
        s_Flt("bar_top_g", m_BarTop[1]);
        s_Flt("bar_top_b", m_BarTop[2]);
        s_Bool("show_icon", m_ShowIcon);
        s_Bool("zoom_enabled", m_ZoomEnabled);
        s_Flt("zoom_max", m_ZoomMax);
        s_Flt("zoom_step", m_ZoomStep);
        s_Flt("zoom_smooth", m_ZoomSmooth);
        s_Bool("zoom_remember", m_ZoomRemember);
        s_Bool("zoom_any_item", m_ZoomAnyItem);
        s_Flt("fill_r", m_FillColor[0]);
        s_Flt("fill_g", m_FillColor[1]);
        s_Flt("fill_b", m_FillColor[2]);
        s_Flt("low_r", m_LowColor[0]);
        s_Flt("low_g", m_LowColor[1]);
        s_Flt("low_b", m_LowColor[2]);

        s_Bool("tint_enabled", m_TintEnabled);
        s_Bool("tint_remove", m_TintRemove);
        s_Flt("tint_r", m_InstinctTint[0]);
        s_Flt("tint_g", m_InstinctTint[1]);
        s_Flt("tint_b", m_InstinctTint[2]);
        s_Flt("lut_strength", m_TintStrength);
        s_Bool("tint_keep_brightness", m_TintKeepBrightness);
        s_Flt("lut_brightness", m_TintBrightness);
        s_Int("lut_barrier", m_LutBarrier);

        s_Bool("sound_enabled", m_SoundEnabled);
        s_Int("sound_source", m_SoundSource);
        s_Flt("sound_volume", m_SoundVolume);
        s_Bool("fade_out_on", m_FadeOutOn);
        s_Flt("fade_out_ms", m_FadeOutMs);
        s_Bool("fade_in_on", m_FadeInOn);
        s_Flt("fade_in_ms", m_FadeInMs);

        for (int i = 0; i < kGlowCatCount; ++i) {
            const std::string s_K = std::string("glow_") + kGlowCats[i].Key + "_";
            const SGlowSetting& s_G = m_Glow[i];
            s_Int((s_K + "mode").c_str(), s_G.Mode);
            s_Bool((s_K + "fill_on").c_str(), s_G.FillOn);
            s_Bool((s_K + "out_on").c_str(), s_G.OutlineOn);
            s_Bool((s_K + "seeded").c_str(), s_G.Seeded);
            const char* s_Ch = "rgba";
            for (int k = 0; k < 4; ++k) {
                s_Flt((s_K + "fill_" + s_Ch[k]).c_str(), s_G.Fill[k]);
                s_Flt((s_K + "out_" + s_Ch[k]).c_str(), s_G.Outline[k]);
            }
        }

        s_Str("preset_name", m_PresetName);
        s_Str("custom_path", m_CustomPath);
        s_Str("action_patterns", m_ActionPatterns);

        s_File.flush();
        if (!s_File.good()) return false;
    }

    fs::rename(s_Tmp, s_Final, s_Ec);
    if (s_Ec) {
        s_Ec.clear();
        fs::copy_file(s_Tmp, s_Final, fs::copy_options::overwrite_existing, s_Ec);
        std::error_code s_Ec2;
        fs::remove(s_Tmp, s_Ec2);
    }
    return !s_Ec;
}

bool BetterInstinct::LoadConfigFile() {
    std::wstring s_Dir = ConfigDir();
    if (s_Dir.empty()) s_Dir = m_ModDir;
    if (s_Dir.empty()) return false;

    std::ifstream s_File(fs::path(s_Dir) / L"BetterInstinct.cfg");
    if (!s_File) return false;

    std::map<std::string, std::string> s_Map;
    std::string s_Line;
    while (std::getline(s_File, s_Line)) {
        while (!s_Line.empty() && (s_Line.back() == '\r' || s_Line.back() == '\n')) s_Line.pop_back();
        const size_t s_Eq = s_Line.find('=');
        if (s_Eq == std::string::npos || s_Eq == 0) continue;
        s_Map[s_Line.substr(0, s_Eq)] = s_Line.substr(s_Eq + 1);
    }
    if (s_Map.empty()) return false;

    auto s_Find = [&](const char* k) -> const std::string* {
        const auto s_It = s_Map.find(k);
        return s_It == s_Map.end() ? nullptr : &s_It->second;
    };
    auto s_Flt = [&](const char* k, float& v, float lo, float hi) {
        if (const std::string* p = s_Find(k)) {
            char* s_End = nullptr;
            const double s_D = std::strtod(p->c_str(), &s_End);
            if (s_End != p->c_str()) v = std::clamp(static_cast<float>(s_D), lo, hi);
        }
    };
    auto s_Int = [&](const char* k, int& v, int lo, int hi) {
        if (const std::string* p = s_Find(k)) {
            char* s_End = nullptr;
            const long s_L = std::strtol(p->c_str(), &s_End, 10);
            if (s_End != p->c_str()) v = std::clamp(static_cast<int>(s_L), lo, hi);
        }
    };
    auto s_Bool = [&](const char* k, bool& v) {
        if (const std::string* p = s_Find(k)) v = (*p == "1");
    };
    auto s_Str = [&](const char* k, char* dst, size_t cap) {
        if (const std::string* p = s_Find(k)) std::snprintf(dst, cap, "%s", p->c_str());
    };

    s_Int("instinct_key", m_InstinctKey, 1, 254);
    s_Bool("toggle_mode", m_ToggleMode);
    s_Bool("block_enabled", m_BlockEnabled);
    s_Flt("focus_scale", m_FocusScale, 0.05f, 0.95f);
    s_Flt("blend_speed", m_BlendSpeed, 1.0f, 30.0f);
    s_Flt("drain_seconds", m_DrainSeconds, 2.0f, 120.0f);
    s_Flt("recharge_seconds", m_RechargeSeconds, 2.0f, 300.0f);
    s_Flt("recharge_delay", m_RechargeDelay, 0.0f, 10.0f);

    s_Bool("hud_enabled", m_HudEnabled);
    s_Bool("hud_always", m_HudAlways);
    s_Flt("hud_width", m_HudWidth, 0.05f, 0.5f);
    s_Flt("hud_y", m_HudYOffset, 0.0f, 1080.0f);
    s_Flt("hud_x", m_HudX, 0.0f, 1.0f);
    s_Bool("hud_vertical", m_HudVertical);
    s_Bool("fire_fx", m_FireFx);
    s_Flt("fire_amount", m_FireAmount, 0.2f, 2.5f);
    s_Flt("hud_scale", m_HudScale, 0.5f, 3.0f);
    s_Flt("hud_opacity", m_HudOpacity, 0.1f, 1.0f);
    s_Flt("bar_bottom_r", m_BarBottom[0], 0.0f, 1.0f);
    s_Flt("bar_bottom_g", m_BarBottom[1], 0.0f, 1.0f);
    s_Flt("bar_bottom_b", m_BarBottom[2], 0.0f, 1.0f);
    s_Flt("bar_top_r", m_BarTop[0], 0.0f, 1.0f);
    s_Flt("bar_top_g", m_BarTop[1], 0.0f, 1.0f);
    s_Flt("bar_top_b", m_BarTop[2], 0.0f, 1.0f);
    s_Bool("show_icon", m_ShowIcon);
    s_Bool("zoom_enabled", m_ZoomEnabled);
    s_Flt("zoom_max", m_ZoomMax, 1.5f, 10.0f);
    s_Flt("zoom_step", m_ZoomStep, 1.03f, 1.6f);
    s_Flt("zoom_smooth", m_ZoomSmooth, 2.0f, 40.0f);
    s_Bool("zoom_remember", m_ZoomRemember);
    s_Bool("zoom_any_item", m_ZoomAnyItem);
    s_Flt("fill_r", m_FillColor[0], 0.0f, 1.0f);
    s_Flt("fill_g", m_FillColor[1], 0.0f, 1.0f);
    s_Flt("fill_b", m_FillColor[2], 0.0f, 1.0f);
    s_Flt("low_r", m_LowColor[0], 0.0f, 1.0f);
    s_Flt("low_g", m_LowColor[1], 0.0f, 1.0f);
    s_Flt("low_b", m_LowColor[2], 0.0f, 1.0f);

    s_Bool("tint_enabled", m_TintEnabled);
    s_Bool("tint_remove", m_TintRemove);
    s_Flt("tint_r", m_InstinctTint[0], 0.0f, 1.0f);
    s_Flt("tint_g", m_InstinctTint[1], 0.0f, 1.0f);
    s_Flt("tint_b", m_InstinctTint[2], 0.0f, 1.0f);
    s_Flt("lut_strength", m_TintStrength, 0.0f, 1.0f);
    s_Bool("tint_keep_brightness", m_TintKeepBrightness);
    s_Flt("lut_brightness", m_TintBrightness, 0.25f, 3.0f);
    s_Int("lut_barrier", m_LutBarrier, 0, 1);

    s_Bool("sound_enabled", m_SoundEnabled);
    s_Int("sound_source", m_SoundSource, 0, 1);
    s_Flt("sound_volume", m_SoundVolume, 0.0f, 1.0f);
    s_Bool("fade_out_on", m_FadeOutOn);
    s_Flt("fade_out_ms", m_FadeOutMs, 0.0f, 3000.0f);
    s_Bool("fade_in_on", m_FadeInOn);
    s_Flt("fade_in_ms", m_FadeInMs, 0.0f, 3000.0f);

    for (int i = 0; i < kGlowCatCount; ++i) {
        const std::string s_K = std::string("glow_") + kGlowCats[i].Key + "_";
        SGlowSetting& s_G = m_Glow[i];
        s_Int((s_K + "mode").c_str(), s_G.Mode, 0, 1);
        s_Bool((s_K + "fill_on").c_str(), s_G.FillOn);
        s_Bool((s_K + "out_on").c_str(), s_G.OutlineOn);
        s_Bool((s_K + "seeded").c_str(), s_G.Seeded);
        const char* s_Ch = "rgba";
        for (int k = 0; k < 4; ++k) {
            s_Flt((s_K + "fill_" + s_Ch[k]).c_str(), s_G.Fill[k], 0.0f, 1.0f);
            s_Flt((s_K + "out_" + s_Ch[k]).c_str(), s_G.Outline[k], 0.0f, 1.0f);
        }
    }

    if (const std::string* p = s_Find("preset_name")) m_PresetName = *p;
    s_Str("custom_path", m_CustomPath, sizeof(m_CustomPath));
    s_Str("action_patterns", m_ActionPatterns, sizeof(m_ActionPatterns));
    if (m_ActionPatterns[0] == '\0') {
        std::snprintf(m_ActionPatterns, sizeof(m_ActionPatterns), "%s", "instinct,focus");
    }
    RebuildPatterns();

    g_Key = m_InstinctKey;
    return true;
}

// ---------------------------------------------------------------------------
// Sound (Windows MCI: wav and mp3, looping, volume fades)
// ---------------------------------------------------------------------------

void BetterInstinct::EnsurePresets() {
    struct Preset { const wchar_t* Name; Samples (*Make)(); };
    const Preset s_Presets[] = {
        {L"Preset - Low Hum.wav", PresetLowHum},
        {L"Preset - Heartbeat.wav", PresetHeartbeat},
        {L"Preset - Pulse Sweep.wav", PresetPulseSweep},
    };

    for (const auto& s_Preset : s_Presets) {
        const fs::path s_Path = fs::path(m_SoundDir) / s_Preset.Name;
        std::error_code s_Ec;
        if (!fs::exists(s_Path, s_Ec)) {
            WriteWav(s_Path, s_Preset.Make(), 0.6f);
        }
    }
}

void BetterInstinct::ScanSounds() {
    m_PresetFiles.clear();
    std::error_code s_Ec;
    for (const auto& s_Entry : fs::directory_iterator(fs::path(m_SoundDir), s_Ec)) {
        std::error_code s_Ec2;
        if (!s_Entry.is_regular_file(s_Ec2)) continue;
        std::wstring s_Ext = s_Entry.path().extension().wstring();
        std::transform(s_Ext.begin(), s_Ext.end(), s_Ext.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        if (s_Ext == L".wav" || s_Ext == L".mp3") {
            m_PresetFiles.push_back(WideToUtf8(s_Entry.path().filename().wstring()));
        }
    }
    std::sort(m_PresetFiles.begin(), m_PresetFiles.end());

    m_PresetIndex = -1;
    for (size_t i = 0; i < m_PresetFiles.size(); ++i) {
        if (m_PresetFiles[i] == m_PresetName) m_PresetIndex = static_cast<int>(i);
    }
    if (m_PresetIndex < 0 && !m_PresetFiles.empty()) {
        m_PresetIndex = 0;
        m_PresetName = m_PresetFiles[0];
    }
}

std::wstring BetterInstinct::CurrentSoundPath() const {
    if (m_SoundSource == 1) {
        return Utf8ToWide(m_CustomPath);
    }
    if (m_PresetIndex >= 0 && m_PresetIndex < static_cast<int>(m_PresetFiles.size())) {
        return m_SoundDir + Utf8ToWide(m_PresetFiles[m_PresetIndex]);
    }
    return L"";
}

bool BetterInstinct::OpenSound() {
    const std::wstring s_Path = CurrentSoundPath();
    std::error_code s_Ec;
    if (s_Path.empty() || !fs::exists(fs::path(s_Path), s_Ec)) {
        m_SoundStatus = "Sound file not found.";
        return false;
    }

    mciSendStringW(L"close instinctsnd", nullptr, 0, nullptr);
    const std::wstring s_Cmd = L"open \"" + s_Path + L"\" type mpegvideo alias instinctsnd";
    if (mciSendStringW(s_Cmd.c_str(), nullptr, 0, nullptr) != 0) {
        m_SoundStatus = "Couldn't open that file (use .wav or .mp3).";
        return false;
    }

    m_SoundStatus.clear();
    m_SndOpen = true;
    return true;
}

void BetterInstinct::CloseSound() {
    if (m_SndOpen) {
        mciSendStringW(L"stop instinctsnd", nullptr, 0, nullptr);
        mciSendStringW(L"close instinctsnd", nullptr, 0, nullptr);
    }
    m_SndOpen = false;
    m_SndPlaying = false;
    m_SndDir = 0;
    m_SndGain = 0.0f;
}

void BetterInstinct::ApplySoundVolume() {
    if (!m_SndOpen) return;
    const int s_Vol = std::clamp(static_cast<int>(m_SndGain * m_SoundVolume * 1000.0f), 0, 1000);
    const std::wstring s_Cmd = L"setaudio instinctsnd volume to " + std::to_wstring(s_Vol);
    mciSendStringW(s_Cmd.c_str(), nullptr, 0, nullptr);
}

void BetterInstinct::StartSound() {
    if (!m_SoundEnabled) return;

    const bool s_FadeIn = m_FadeInOn && m_FadeInMs > 0.0f;

    if (!m_SndPlaying) {
        if (!m_SndOpen && !OpenSound()) return;
        m_SndGain = s_FadeIn ? 0.0f : 1.0f;
        ApplySoundVolume();
        mciSendStringW(L"play instinctsnd repeat", nullptr, 0, nullptr);
        m_SndPlaying = true;
        m_SndDir = s_FadeIn ? 1 : 0;
    } else if (s_FadeIn) {
        m_SndDir = 1; // was fading out: fade back in from where it is
    } else {
        m_SndGain = 1.0f;
        m_SndDir = 0;
        ApplySoundVolume();
    }
}

void BetterInstinct::StopSound() {
    if (!m_SndPlaying) return;

    if (m_FadeOutOn && m_FadeOutMs > 0.0f) {
        m_SndDir = -1;
    } else {
        CloseSound();
    }
}

// Runs every frame from the UI callback, so sound also stops cleanly when you leave the mission.
void BetterInstinct::UpdateSound(float p_Dt, bool p_InPlay) {
    const bool s_Wanted = m_SoundEnabled && ((m_Active.load() && p_InPlay) || m_Testing);

    if (s_Wanted && !m_SoundWasWanted) {
        StartSound();
    } else if (!s_Wanted && m_SoundWasWanted) {
        StopSound();
    }
    m_SoundWasWanted = s_Wanted;

    if (m_SndPlaying && m_SndDir != 0) {
        if (m_SndDir > 0) {
            m_SndGain += p_Dt / std::max(m_FadeInMs * 0.001f, 0.001f);
            if (m_SndGain >= 1.0f) { m_SndGain = 1.0f; m_SndDir = 0; }
            ApplySoundVolume();
        } else {
            m_SndGain -= p_Dt / std::max(m_FadeOutMs * 0.001f, 0.001f);
            if (m_SndGain <= 0.0f) {
                CloseSound();
            } else {
                ApplySoundVolume();
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Input helpers
// ---------------------------------------------------------------------------

bool BetterInstinct::IsKeyDown(int p_Vk) {
    return p_Vk != 0 && (GetAsyncKeyState(p_Vk) & 0x8000) != 0;
}

bool BetterInstinct::InstinctKeyDown() const {
    return PhysKeyDown(m_InstinctKey);
}

// Makes the game think the Instinct key was released (or, in toggle mode, taps it to switch Instinct off).
void BetterInstinct::ForceReleaseInstinct() {
    const auto s_Variants = KeyVariants(m_InstinctKey);
    if (m_ToggleMode) {
        SendKey(s_Variants[0], false);
        SendKey(s_Variants[0], true);
        return;
    }

    for (int s_Vk : s_Variants) {
        if (PhysKeyDown(s_Vk)) SendKey(s_Vk, true);
    }
}

std::string BetterInstinct::KeyName(int p_Vk) {
    switch (p_Vk) {
        case VK_LBUTTON: return "Left Mouse";
        case VK_RBUTTON: return "Right Mouse";
        case VK_MBUTTON: return "Middle Mouse";
        case VK_XBUTTON1: return "Mouse 4";
        case VK_XBUTTON2: return "Mouse 5";
        case VK_SHIFT: return "Shift (either)";
        case VK_CONTROL: return "Ctrl (either)";
        case VK_MENU: return "Alt (either)";
        default: break;
    }

    UINT s_Scan = MapVirtualKeyW(static_cast<UINT>(p_Vk), MAPVK_VK_TO_VSC);
    switch (p_Vk) { // keys that need the "extended" bit to get the right name
        case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
        case VK_PRIOR: case VK_NEXT: case VK_END: case VK_HOME:
        case VK_INSERT: case VK_DELETE: case VK_DIVIDE: case VK_NUMLOCK:
        case VK_RCONTROL: case VK_RMENU:
            s_Scan |= 0x100;
            break;
        default: break;
    }

    wchar_t s_Name[64] = {};
    if (s_Scan != 0 && GetKeyNameTextW(static_cast<LONG>(s_Scan << 16), s_Name, 64) > 0) {
        char s_Utf8[128] = {};
        WideCharToMultiByte(CP_UTF8, 0, s_Name, -1, s_Utf8, sizeof(s_Utf8), nullptr, nullptr);
        return s_Utf8;
    }

    char s_Fallback[16];
    std::snprintf(s_Fallback, sizeof(s_Fallback), "Key 0x%02X", p_Vk);
    return s_Fallback;
}

// ---------------------------------------------------------------------------
// Instinct glow colors (people and items)
//
// The game keeps the Instinct outline / silhouette colors as properties of a config entity (the same ones other
// Instinct mods change with "entity patches": m_GlowCivilianColor, m_GlowEnemiesOutlineColor, ...). We find that entity at
// runtime by looking for those property names, remember where each color lives, and write our values into it.
// The game's own values are remembered, so "Game default" puts them back exactly.
// ---------------------------------------------------------------------------

namespace {

bool CanWriteMemory(const void* p_Ptr, size_t p_Size) {
    MEMORY_BASIC_INFORMATION s_Info{};
    if (!p_Ptr || !VirtualQuery(p_Ptr, &s_Info, sizeof(s_Info))) return false;
    if (s_Info.State != MEM_COMMIT) return false;
    if (s_Info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    const DWORD s_Writable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (!(s_Info.Protect & s_Writable)) return false;
    const uint8_t* s_End = static_cast<const uint8_t*>(p_Ptr) + p_Size;
    return s_End <= static_cast<const uint8_t*>(s_Info.BaseAddress) + s_Info.RegionSize;
}

inline void HashMix(uint32_t& p_H, uint32_t p_V) {
    p_H ^= p_V + 0x9E3779B9u + (p_H << 6) + (p_H >> 2);
}

inline uint32_t FloatBits(float p_F) {
    uint32_t s_B = 0;
    std::memcpy(&s_B, &p_F, sizeof(s_B));
    return s_B;
}

} // namespace

// One short log file (BetterInstinct.log, next to the config) with the result of the last color search.
static void GlowLog(const std::string& p_Text) {
    try {
        std::wstring s_Dir = ConfigDir();
        if (s_Dir.empty()) return;
        std::ofstream s_File(fs::path(s_Dir) / L"BetterInstinct.log", std::ios::trunc);
        s_File << p_Text << "\n";
    } catch (...) {
    }
}

void BetterInstinct::InitGlowDefaults() {
    for (int i = 0; i < kGlowCatCount; ++i) {
        SGlowSetting& s_G = m_Glow[i];
        s_G = SGlowSetting{};
        const int s_F = kGlowCats[i].FillRgb;
        const int s_O = kGlowCats[i].OutlineRgb;
        s_G.Fill[0] = ((s_F >> 16) & 0xFF) / 255.0f;
        s_G.Fill[1] = ((s_F >> 8) & 0xFF) / 255.0f;
        s_G.Fill[2] = (s_F & 0xFF) / 255.0f;
        s_G.Fill[3] = kGlowDefFillAlpha;
        s_G.Outline[0] = ((s_O >> 16) & 0xFF) / 255.0f;
        s_G.Outline[1] = ((s_O >> 8) & 0xFF) / 255.0f;
        s_G.Outline[2] = (s_O & 0xFF) / 255.0f;
        s_G.Outline[3] = kGlowDefOutlineAlpha;
    }
}

uint32_t BetterInstinct::GlowSignature() const {
    uint32_t s_H = 0x811C9DC5u;
    for (int i = 0; i < kGlowCatCount; ++i) {
        const SGlowSetting& s_G = m_Glow[i];
        HashMix(s_H, static_cast<uint32_t>(s_G.Mode));
        HashMix(s_H, (s_G.FillOn ? 1u : 0u) | (s_G.OutlineOn ? 2u : 0u));
        for (int k = 0; k < 4; ++k) {
            HashMix(s_H, FloatBits(s_G.Fill[k]));
            HashMix(s_H, FloatBits(s_G.Outline[k]));
        }
    }
    return s_H;
}

// Called from the activation hook for every entity, before the game activates it. For the Instinct glow config entity
// this writes the player's colors into the entity's memory, so the game picks them up the same way it would pick up a
// data patch (a patch changes the values before they are read; changing them later does not always reach the renderer).
void BetterInstinct::ApplyGlowOnActivate(const ZEntityRef* p_Ref) {
    if (!p_Ref) return;

    ZEntityImpl* s_Entity = nullptr;
    ZEntityType* s_Type = nullptr;
    auto s_ReadType = [&]() {
        s_Entity = p_Ref->GetEntity();
        s_Type = s_Entity ? s_Entity->GetType() : nullptr;
        if (s_Type && !s_Type->m_pPropertyData) s_Type = nullptr;
    };
    if (!SafeCall(s_ReadType) || !s_Type) return;

    {
        std::lock_guard<std::mutex> s_Lock(m_CatchMutex);
        const auto s_Cached = m_CatchTypeCache.find(s_Type);
        if (s_Cached == m_CatchTypeCache.end() || !s_Cached->second) return; // not the glow config entity
    }

    uint32_t s_Written = 0;
    auto s_Apply = [&]() {
        for (int i = 0; i < kGlowCatCount; ++i) {
            const SGlowSetting& s_G = m_Glow[i];
            for (const char* s_Stem : kGlowCats[i].Names) {
                if (!s_Stem) continue;
                for (int s_Out = 0; s_Out < 2; ++s_Out) {
                    const std::string s_Name = std::string(s_Stem) + (s_Out ? "OutlineColor" : "Color");
                    SPropertyData* s_Data = s_Type->FindProperty(Hash::Crc32(s_Name.c_str()));
                    if (!s_Data) continue;

                    const SPropertyInfo& s_Info = s_Data->GetPropertyInfo()->m_propertyInfo;
                    if (s_Info.m_Flags & EPropertyInfoFlags::E_HAS_GETTER_SETTER) continue; // no plain memory behind it
                    const IType* s_TypeInfo = s_Info.m_Type ? s_Info.m_Type->GetTypeInfo() : nullptr;
                    const uint32_t s_Size = s_TypeInfo ? s_TypeInfo->m_nTypeSize : 0;
                    if (s_Size != 16 && s_Size != 12) continue;

                    uint8_t* s_Ptr = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(p_Ref->m_pObj) + s_Data->m_nPropertyOffset);
                    if (!CanWriteMemory(s_Ptr, s_Size)) continue;

                    // Remember the game's own value the first time we see this address; later passes must not mistake our
                    // colors for the game's.
                    std::array<float, 4> s_Van = {0.0f, 0.0f, 0.0f, 1.0f};
                    {
                        std::lock_guard<std::mutex> s_Lock(m_PreMutex);
                        const auto s_Known = m_PreVanilla.find(reinterpret_cast<uintptr_t>(s_Ptr));
                        if (s_Known != m_PreVanilla.end()) {
                            s_Van = s_Known->second;
                        } else {
                            float s_Raw[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                            std::memcpy(s_Raw, s_Ptr, s_Size);
                            bool s_Sane = true;
                            for (int k = 0; k < 4; ++k) {
                                if (!std::isfinite(s_Raw[k]) || s_Raw[k] < -0.01f || s_Raw[k] > 16.0f) s_Sane = false;
                            }
                            if (!s_Sane) continue; // does not look like a color: leave it alone
                            for (int k = 0; k < 4; ++k) s_Van[k] = s_Raw[k];
                            m_PreVanilla[reinterpret_cast<uintptr_t>(s_Ptr)] = s_Van;
                        }
                    }

                    float s_V[4];
                    if (s_G.Mode == 0) {
                        for (int k = 0; k < 4; ++k) s_V[k] = s_Van[k];
                    } else {
                        const float* s_Src = s_Out ? s_G.Outline : s_G.Fill;
                        const bool s_On = s_Out ? s_G.OutlineOn : s_G.FillOn;
                        std::memcpy(s_V, s_Src, sizeof(s_V));
                        if (!s_On) s_V[3] = 0.0f;
                    }
                    std::memcpy(s_Ptr, s_V, s_Size);
                    ++s_Written;
                }
            }
        }
    };
    SafeCall(s_Apply);
    if (s_Written > 0) m_GlowPreApplied.fetch_add(s_Written);
}

// ---------------------------------------------------------------------------
// Better Zoom
// ---------------------------------------------------------------------------
// Camera profile entities (template 00852016D29CC8A3): OTS_Master01, OTSHigh01, OTSLow, CoverHighOTS01, CoverLowOTS01.
// Their m_fFOV is the aim camera's field of view. A smaller FOV = more zoom.

void BetterInstinct::CheckTemplateForZoom(ZTemplateEntityFactory* p_Factory, ZEntityType** p_Entity) {
    if (!p_Factory || !p_Entity) return;

    static constexpr uint64_t s_Ids[] = {
        0x986AC0B7847DC618ULL, // OTS_Master01
        0xB0EA3D50DCBB1A7AULL, // OTSHigh01
        0x6793005D38EFC567ULL, // OTSLow
        0x0997FA17BEB4FBB1ULL, // CoverHighOTS01
        0xC21FA7B9826A5E20ULL, // CoverLowOTS01
    };
    std::vector<ZEntityRef> s_Hits;
    auto s_Look = [&]() {
        auto* s_Blueprint = static_cast<IEntityBlueprintFactory*>(p_Factory->m_blueprintResource.GetResourceData());
        if (!s_Blueprint) return;
        const int64_t s_Count = s_Blueprint->GetSubEntitiesCount();
        if (s_Count <= 0 || s_Count > 2000000) return;
        for (int64_t i = 0; i < s_Count; ++i) {
            const uint64_t s_Id = s_Blueprint->GetSubEntityId(static_cast<int>(i));
            bool s_Match = false;
            for (const uint64_t s_Wanted : s_Ids) {
                if (s_Id == s_Wanted) s_Match = true;
            }
            if (!s_Match) continue;
            ZEntityType** s_Sub = s_Blueprint->GetSubEntity(p_Entity, static_cast<int>(i));
            if (s_Sub) s_Hits.push_back(ZEntityRef(s_Sub));
        }
    };
    if (!SafeCall(s_Look) || s_Hits.empty()) return;

    static const uint32_t s_FovId = Hash::Crc32("m_fFOV");
    for (const ZEntityRef& s_Ref : s_Hits) {
        float s_Fov = 0.0f;
        auto s_Read = [&]() {
            ZEntityImpl* s_Entity = s_Ref.GetEntity();
            ZEntityType* s_Type = s_Entity ? s_Entity->GetType() : nullptr;
            if (!s_Type || !s_Type->m_pPropertyData) return;
            SPropertyData* s_Data = s_Type->FindProperty(s_FovId);
            if (!s_Data) return;
            if (s_Data->GetPropertyInfo()->m_propertyInfo.m_Flags & EPropertyInfoFlags::E_HAS_GETTER_SETTER) return;
            const uint8_t* s_Ptr = reinterpret_cast<const uint8_t*>(reinterpret_cast<uintptr_t>(s_Ref.m_pObj) + s_Data->m_nPropertyOffset);
            if (!CanWriteMemory(s_Ptr, sizeof(float))) return;
            std::memcpy(&s_Fov, s_Ptr, sizeof(float));
        };
        if (!SafeCall(s_Read) || !std::isfinite(s_Fov) || s_Fov < 5.0f || s_Fov > 120.0f) continue; // not a sane FOV: leave it alone

        std::lock_guard<std::mutex> s_Lock(m_ZoomMutex);
        bool s_Known = false;
        for (SZoomProfile& s_P : m_ZoomProfiles) {
            if (s_P.Ref.GetEntity() == s_Ref.GetEntity()) { s_P.VanillaFov = s_Fov; s_Known = true; }
        }
        if (!s_Known) {
            SZoomProfile s_New;
            s_New.Ref = s_Ref;
            s_New.VanillaFov = s_Fov; // read before we ever write, so this is the game's own value
            m_ZoomProfiles.push_back(s_New);
        }
        m_ZoomFound.store(static_cast<uint32_t>(m_ZoomProfiles.size()));
    }
}

// Is the zoom allowed for what 47 holds right now? Wanted: hands empty, or an item that can't be thrown.
// Guns (and throwable items) keep the game's own aiming.
// TODO: wire this to the held item. It needs the item/inventory API of the SDK fork (ZHitman5.h / ZItem.h).
// Until then the check is the "Zoom with any item" option, so the feature is switched off by default.
bool BetterInstinct::HeldItemAllowsZoom() {
    return m_ZoomAnyItem;
}

void BetterInstinct::WriteZoomFov(float p_Zoom) {
    static const uint32_t s_FovId = Hash::Crc32("m_fFOV");
    std::lock_guard<std::mutex> s_Lock(m_ZoomMutex);
    for (SZoomProfile& s_P : m_ZoomProfiles) {
        float s_Fov = s_P.VanillaFov;
        if (p_Zoom > 1.0005f) {
            const float s_Half = s_P.VanillaFov * 0.5f * 3.14159265f / 180.0f;
            s_Fov = 2.0f * std::atan(std::tan(s_Half) / p_Zoom) * 180.0f / 3.14159265f; // real magnification, not just FOV / zoom
            s_Fov = std::max(s_Fov, 1.0f);
        }
        try {
            if (!s_P.Ref.GetEntity()) continue;
            s_P.Ref.SetProperty<float>(s_FovId, s_Fov, true);
            ++m_ZoomWrites;
        } catch (...) {
        }
    }
}

// Diagnostic for the held-item check: lists the properties (name, size, offset, first bytes) of the player, its character,
// the inventory controllers and the items it owns, so the right one can be found. Written to BetterInstinct.log.
void BetterInstinct::DumpHeldItemInfo() {
    std::string s_Out = "Held item dump (hold the item you want to check while you press the button)\n";
    auto s_Dump = [&](const std::string& p_Title, ZEntityRef p_Ref) {
        s_Out += "\n== " + p_Title + " ==\n";
        auto s_Body = [&]() {
            ZEntityImpl* s_Entity = p_Ref.GetEntity();
            ZEntityType* s_Type = s_Entity ? s_Entity->GetType() : nullptr;
            if (!s_Type || !s_Type->m_pPropertyData) { s_Out += "(no properties)\n"; return; }
            int s_N = 0;
            for (const SPropertyData& s_P : *s_Type->m_pPropertyData) {
                if (++s_N > 300) break;
                const SNamedPropertyInfo* s_NI = s_P.GetPropertyInfo();
                if (!s_NI || !s_NI->m_pszPropertyName) continue;
                const SPropertyInfo& s_Info = s_NI->m_propertyInfo;
                const IType* s_T = s_Info.m_Type ? s_Info.m_Type->GetTypeInfo() : nullptr;
                const uint32_t s_Size = s_T ? s_T->m_nTypeSize : 0;
                const bool s_Setter = (s_Info.m_Flags & EPropertyInfoFlags::E_HAS_GETTER_SETTER) != 0;
                char s_Line[256];
                std::snprintf(s_Line, sizeof(s_Line), "%s  size=%u offset=0x%X%s", s_NI->m_pszPropertyName, s_Size,
                              static_cast<unsigned>(s_P.m_nPropertyOffset), s_Setter ? " getter/setter" : "");
                s_Out += s_Line;
                if (!s_Setter && s_Size > 0 && s_Size <= 16) {
                    const uint8_t* s_Ptr = reinterpret_cast<const uint8_t*>(reinterpret_cast<uintptr_t>(p_Ref.m_pObj) + s_P.m_nPropertyOffset);
                    if (CanWriteMemory(s_Ptr, s_Size)) {
                        s_Out += "  value=";
                        for (uint32_t k = 0; k < s_Size; ++k) {
                            char s_Hex[4];
                            std::snprintf(s_Hex, sizeof(s_Hex), "%02X", s_Ptr[k]);
                            s_Out += s_Hex;
                        }
                    }
                }
                s_Out += "\n";
            }
        };
        if (!SafeCall(s_Body)) s_Out += "(read failed)\n";
    };

    try {
        auto s_Local = SDK()->GetLocalPlayer();
        if (!s_Local.m_pInterfaceRef) { s_Out += "No local player.\n"; GlowLog(s_Out); return; }
        s_Dump("ZHitman5", s_Local.m_entityRef);

        auto& s_CharRef = s_Local.m_pInterfaceRef->m_pCharacter;
        if (s_CharRef.m_pInterfaceRef) {
            s_Dump("ZCharacter", s_CharRef.m_entityRef);
            auto& s_ContRef = s_CharRef.m_pInterfaceRef->m_rSubcontrollerContainer;
            if (s_ContRef.m_pInterfaceRef) {
                s_Dump("SubcontrollerContainer", s_ContRef.m_entityRef);
                int s_Idx = 0;
                for (auto& s_Sub : s_ContRef.m_pInterfaceRef->m_aReferencedControllers) {
                    if (s_Idx >= 40) break;
                    s_Dump("Subcontroller " + std::to_string(s_Idx++), s_Sub.m_entityRef);
                }
            }
        }
    } catch (...) {
        s_Out += "\nException while reading.\n";
    }
    GlowLog(s_Out);
}

void BetterInstinct::UpdateZoom(float p_Dt) {
    if (m_DumpRequest.exchange(false)) DumpHeldItemInfo();
    const bool s_Focused = GameIsForeground() && !m_Binding;
    const bool s_RightDown = s_Focused && g_Phys[VK_RBUTTON].load();
    const bool s_Gate = m_ZoomEnabled && s_RightDown && HeldItemAllowsZoom();
    g_ZoomWanted.store(s_Gate);

    const int s_Delta = g_Wheel.exchange(0);
    if (s_Gate && s_Delta != 0) {
        m_ZoomTarget *= std::pow(m_ZoomStep, static_cast<float>(s_Delta) / 120.0f);
    }
    m_ZoomTarget = std::clamp(m_ZoomTarget, 1.0f, std::max(1.0f, m_ZoomMax));
    if (!s_RightDown && !m_ZoomRemember) m_ZoomTarget = 1.0f;

    const float s_Wanted = s_Gate ? m_ZoomTarget : 1.0f; // not allowed right now: the game's own FOV
    m_ZoomCur += (s_Wanted - m_ZoomCur) * std::clamp(m_ZoomSmooth * p_Dt, 0.0f, 1.0f);
    if (std::abs(m_ZoomCur - s_Wanted) < 0.002f) m_ZoomCur = s_Wanted;

    if (std::abs(m_ZoomCur - m_ZoomWritten) > 0.002f) {
        WriteZoomFov(m_ZoomCur);
        m_ZoomWritten = m_ZoomCur;
    }
}

// The glow config entity is the same one the Killer Instinct data mod patches: entity f55806ff6135965f in the main game's
// template (0092AAB2AF8AC127) and d796d60af9226450 (plus ...6400) in Sniper Assassin's (0029C0633A11CDE1).
DEFINE_PLUGIN_DETOUR(BetterInstinct, void, ZTemplateEntityFactory_ConfigureEntity, ZTemplateEntityFactory* th, ZEntityType** pEntity, const SExternalReferences& externalRefs, uint8_t* unk0) {
    p_Hook->CallOriginal(th, pEntity, externalRefs, unk0); // the game builds the entities and sets their properties first
    ++m_GlowTplCalls;
    CheckTemplateForGlow(th, pEntity);
    CheckTemplateForZoom(th, pEntity);
    return HookResult<void>(HookAction::Return());
}

void BetterInstinct::CheckTemplateForGlow(ZTemplateEntityFactory* p_Factory, ZEntityType** p_Entity) {
    if (!p_Factory || !p_Entity) return;

    static constexpr uint64_t s_Ids[] = {0xF55806FF6135965FULL, 0xD796D60AF9226450ULL, 0xD796D60AF9226400ULL};
    std::vector<ZEntityRef> s_Hits;
    auto s_Look = [&]() {
        auto* s_Blueprint = static_cast<IEntityBlueprintFactory*>(p_Factory->m_blueprintResource.GetResourceData());
        if (!s_Blueprint) return;
        const int64_t s_Count = s_Blueprint->GetSubEntitiesCount();
        if (s_Count <= 0 || s_Count > 2000000) return;
        for (int64_t i = 0; i < s_Count; ++i) {
            const uint64_t s_Id = s_Blueprint->GetSubEntityId(static_cast<int>(i)); // cheap: no entity is touched unless the id matches
            bool s_Match = false;
            for (const uint64_t s_Wanted : s_Ids) {
                if (s_Id == s_Wanted) s_Match = true;
            }
            if (!s_Match) continue;
            ZEntityType** s_Sub = s_Blueprint->GetSubEntity(p_Entity, static_cast<int>(i));
            if (s_Sub) s_Hits.push_back(ZEntityRef(s_Sub));
        }
    };
    if (!SafeCall(s_Look)) return;

    for (const ZEntityRef& s_Ref : s_Hits) {
        ++m_GlowTplHits;
        CatchGlowEntity(&s_Ref);      // remembers it (the color search and the live updates use this list)
        ApplyGlowOnActivate(&s_Ref);  // writes the player's colors right now
    }
}

// Finds every glow color in the game's entities. Runs on the game thread (it reads the entity manager).
void BetterInstinct::ScanGlowSlots(bool p_KeepSnapshots) {
    std::vector<SGlowSlot> s_Old;
    if (p_KeepSnapshots) s_Old = m_GlowSlots;
    m_GlowSlots.clear();

    uint32_t s_Scanned = 0, s_Entities = 0, s_Skipped = 0;
    std::string s_Detail; // one line per matching property, written to the log

    try {
        if (!Globals::EntityManager) {
            m_GlowState = 0;
            m_GlowFound = 0;
            return;
        }

        struct SProp { uint32_t Id; int Cat; bool Outline; };
        std::vector<SProp> s_Props;
        std::vector<uint32_t> s_Anchors; // a few properties that every glow config entity has
        for (int i = 0; i < kGlowCatCount; ++i) {
            for (const char* s_Stem : kGlowCats[i].Names) {
                if (!s_Stem) continue;
                const std::string s_Fill = std::string(s_Stem) + "Color";
                const std::string s_Out = std::string(s_Stem) + "OutlineColor";
                s_Props.push_back({Hash::Crc32(s_Fill.c_str()), i, false});
                s_Props.push_back({Hash::Crc32(s_Out.c_str()), i, true});
                if (i == 0 || i == 3 || i == 4 || i == 10) s_Anchors.push_back(s_Props[s_Props.size() - 2].Id);
            }
        }

        std::set<ZEntityImpl*> s_Visited;
        uint32_t s_Candidates = 0;
        std::set<ZEntityType*> s_SeenTypes;
        std::map<ZEntityType*, std::pair<int64_t, uint32_t>> s_InstinctTypes; // type -> (offset, size) of m_pInstinctControl
        std::vector<ZEntityRef> s_Pending;                                    // entities that m_pInstinctControl points at
        std::string s_LinkInfo;
        std::string s_Cand;
        auto s_Process = [&](const ZEntityRef& s_Ref) {
            ++s_Scanned;
            ZEntityImpl* s_Entity = s_Ref.GetEntity();
            if (!s_Entity) return;
            if (!s_Visited.insert(s_Entity).second) return; // already looked at
            ZEntityType* s_Type = s_Entity->GetType();
            if (!s_Type || !s_Type->m_pPropertyData) return;

            // Diagnostics, once per entity type: which types have properties named like Instinct colors?
            if (s_SeenTypes.insert(s_Type).second && s_Candidates < 40) {
                std::string s_Names;
                uint32_t s_N = 0;
                for (const SPropertyData& s_P : *s_Type->m_pPropertyData) {
                    const SNamedPropertyInfo* s_NI = s_P.GetPropertyInfo();
                    if (!s_NI || !s_NI->m_pszPropertyName) continue;
                    std::string s_Name = s_NI->m_pszPropertyName;
                    std::string s_Lower = s_Name;
                    for (char& s_C : s_Lower) s_C = static_cast<char>(std::tolower(static_cast<unsigned char>(s_C)));
                    if (s_Name == "m_pInstinctControl") {
                        const IType* s_LinkType = s_NI->m_propertyInfo.m_Type ? s_NI->m_propertyInfo.m_Type->GetTypeInfo() : nullptr;
                        s_InstinctTypes[s_Type] = {s_P.m_nPropertyOffset, s_LinkType ? s_LinkType->m_nTypeSize : 0};
                    }
                    if (s_Lower.find("color") != std::string::npos &&
                        (s_Lower.find("glow") != std::string::npos || s_Lower.find("outline") != std::string::npos ||
                         s_Lower.find("instinct") != std::string::npos)) {
                        if (++s_N <= 60) s_Names += s_Name + " ";
                    }
                }
                if (s_N > 0) {
                    ++s_Candidates;
                    s_Cand += "  type with " + std::to_string(s_N) + " matching properties: " + s_Names + "\n";
                }
            }

            {
                const auto s_Link = s_InstinctTypes.find(s_Type);
                if (s_Link != s_InstinctTypes.end() && (s_Link->second.second == 16 || s_Link->second.second == 8)) {
                    ZEntityType** s_Target = *reinterpret_cast<ZEntityType***>(
                        reinterpret_cast<uintptr_t>(s_Ref.m_pObj) + s_Link->second.first);
                    if (s_Target) s_Pending.push_back(ZEntityRef(s_Target));
                }
            }

            bool s_IsConfig = false;
            for (const uint32_t s_Id : s_Anchors) {
                if (s_Type->FindProperty(s_Id)) { s_IsConfig = true; break; }
            }
            if (!s_IsConfig) {
                return;
            }
            ++s_Entities;

            for (const SProp& s_Prop : s_Props) {
                SPropertyData* s_Data = s_Type->FindProperty(s_Prop.Id);
                if (!s_Data) continue;

                const SPropertyInfo& s_Info = s_Data->GetPropertyInfo()->m_propertyInfo;
                const bool s_ViaSetter = (s_Info.m_Flags & EPropertyInfoFlags::E_HAS_GETTER_SETTER) != 0;

                const IType* s_TypeInfo = s_Info.m_Type ? s_Info.m_Type->GetTypeInfo() : nullptr;
                const uint32_t s_Size = s_TypeInfo ? s_TypeInfo->m_nTypeSize : 0;
                s_Detail += "  " + std::string(kGlowCats[s_Prop.Cat].Key) + (s_Prop.Outline ? " outline" : " fill") +
                            ": size=" + std::to_string(s_Size) + (s_ViaSetter ? " getter/setter" : " direct") + "\n";
                if (s_Size != 16 && s_Size != 12) { ++s_Skipped; s_Detail += "    -> skipped (not a color type)\n"; continue; }

                // Properties with a getter/setter have no plain memory behind them: those are only changed through
                // SetProperty (Ptr stays null).
                uint8_t* s_Ptr = nullptr;
                if (!s_ViaSetter) {
                    s_Ptr = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(s_Ref.m_pObj) + s_Data->m_nPropertyOffset);
                    if (!CanWriteMemory(s_Ptr, s_Size)) { ++s_Skipped; s_Detail += "    -> skipped (memory not writable)\n"; continue; }

                    bool s_Dup = false;
                    for (const SGlowSlot& s_Existing : m_GlowSlots) {
                        if (s_Existing.Ptr == s_Ptr) { s_Dup = true; break; }
                    }
                    if (s_Dup) continue;
                }

                SGlowSlot s_Slot;
                s_Slot.Ptr = s_Ptr;
                s_Slot.Size = s_Size;
                s_Slot.Ref = s_Ref;
                s_Slot.PropId = s_Prop.Id;
                s_Slot.Cat = s_Prop.Cat;
                s_Slot.Outline = s_Prop.Outline;

                bool s_FromOld = false;
                for (const SGlowSlot& s_OldSlot : s_Old) {
                    if (s_Ptr ? (s_OldSlot.Ptr == s_Ptr)
                              : (!s_OldSlot.Ptr && s_OldSlot.PropId == s_Prop.Id && s_OldSlot.Ref.GetEntity() == s_Ref.GetEntity())) {
                        std::memcpy(s_Slot.Vanilla, s_OldSlot.Vanilla, sizeof(s_Slot.Vanilla));
                        s_FromOld = true;
                        break;
                    }
                }
                if (!s_FromOld && !s_Ptr) {
                    // No readable memory: the game's colors are taken from the built-in table.
                    const int s_Rgb = s_Prop.Outline ? kGlowCats[s_Prop.Cat].OutlineRgb : kGlowCats[s_Prop.Cat].FillRgb;
                    s_Slot.Vanilla[0] = ((s_Rgb >> 16) & 0xFF) / 255.0f;
                    s_Slot.Vanilla[1] = ((s_Rgb >> 8) & 0xFF) / 255.0f;
                    s_Slot.Vanilla[2] = (s_Rgb & 0xFF) / 255.0f;
                    s_Slot.Vanilla[3] = s_Prop.Outline ? kGlowDefOutlineAlpha : kGlowDefFillAlpha;
                } else if (!s_FromOld) {
                    float s_Raw[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                    std::memcpy(s_Raw, s_Ptr, s_Size);
                    {   // the game's own value remembered at activation (memory may already hold our colors)
                        std::lock_guard<std::mutex> s_PreLock(m_PreMutex);
                        const auto s_PreIt = m_PreVanilla.find(reinterpret_cast<uintptr_t>(s_Ptr));
                        if (s_PreIt != m_PreVanilla.end()) for (int k = 0; k < 4; ++k) s_Raw[k] = s_PreIt->second[k];
                    }
                    bool s_Sane = true;
                    for (int k = 0; k < 4; ++k) {
                        if (!std::isfinite(s_Raw[k]) || s_Raw[k] < -0.01f || s_Raw[k] > 16.0f) s_Sane = false;
                    }
                    if (!s_Sane) { ++s_Skipped; s_Detail += "    -> skipped (value doesn't look like a color)\n"; continue; } // leave it alone
                    std::memcpy(s_Slot.Vanilla, s_Raw, sizeof(s_Slot.Vanilla));
                }
                m_GlowSlots.push_back(s_Slot);
            }
        };

        uint32_t s_Faults = 0;
        auto s_Guarded = [&](const ZEntityRef& p_Ref) {
            auto s_Run = [&]() { s_Process(p_Ref); };
            if (!SafeCall(s_Run)) ++s_Faults;
        };

        // Source 0: entities caught by the activation hook (this is where the config entity normally comes from).
        uint32_t s_FromCaught = 0;
        {
            std::vector<ZEntityRef> s_Caught;
            {
                std::lock_guard<std::mutex> s_Lock(m_CatchMutex);
                s_Caught = m_GlowCaught;
            }
            for (const ZEntityRef& s_C : s_Caught) { s_Guarded(s_C); ++s_FromCaught; }
        }
        s_Detail += "Entities caught at activation: " + std::to_string(s_FromCaught) + "\n";

        // Source 1: the entity manager's list (only part of the scene). Skipped when the hook already found the config.
        if (s_Entities == 0)
            for (const auto& s_Pair : Globals::EntityManager->m_Entities) s_Guarded(s_Pair.second);
        const uint32_t s_FromManager = s_Scanned;

        // Source 2: every entity of the loaded scene and its bricks, walked through the blueprint factories
        // (this is where the static Instinct config entity lives).
        // Nested templates (a sub-entity that is itself the root of another template) are walked too, because the
        // Instinct config entity lives inside one of them, not directly in a top-level brick.
        std::set<ZEntityImpl*> s_WalkedRoots;
        uint32_t s_NestedRoots = 0;
        std::function<void(const ZEntityRef&, int)> s_WalkRoot = [&](const ZEntityRef& p_Root, int p_Depth) {
            ZEntityBlueprintFactoryBase* s_Factory = nullptr;
            int64_t s_Count = 0;
            auto s_GetFactory = [&]() {
                if (!p_Root) return;
                s_Factory = p_Root.GetBlueprintFactory();
                if (s_Factory) s_Count = s_Factory->GetSubEntitiesCount();
            };
            if (!SafeCall(s_GetFactory)) { ++s_Faults; return; }
            s_Guarded(p_Root);
            if (!s_Factory || s_Count <= 0 || s_Count > 2000000) return;
            for (int64_t i = 0; i < s_Count; ++i) {
                ZEntityType** s_Sub = nullptr;
                auto s_GetSub = [&]() { s_Sub = s_Factory->GetSubEntity(p_Root.m_pObj, static_cast<int>(i)); };
                if (!SafeCall(s_GetSub)) { ++s_Faults; break; } // this brick's factory is not usable (yet)
                if (!s_Sub) continue;
                const ZEntityRef s_SubRef(s_Sub);
                s_Guarded(s_SubRef);

                if (p_Depth >= 48) continue;
                bool s_IsNestedRoot = false;
                ZEntityImpl* s_SubEntity = nullptr;
                auto s_Check = [&]() {
                    s_SubEntity = s_SubRef.GetEntity();
                    if (!s_SubEntity || s_SubEntity == p_Root.GetEntity()) return;
                    // A template root is flagged on its entity type; testing the flag directly avoids QueryInterface,
                    // which crashes on ordinary entities.
                    const ZEntityType* s_SubType = s_SubEntity->GetType();
                    s_IsNestedRoot = s_SubType && (s_SubType->m_nBorrowedPointersMask & 0x200) != 0;
                };
                if (!SafeCall(s_Check)) { ++s_Faults; continue; }
                if (s_IsNestedRoot && s_WalkedRoots.insert(s_SubEntity).second) {
                    ++s_NestedRoots;
                    s_WalkRoot(s_SubRef, p_Depth + 1);
                }
            }
        };

        // Follow the Instinct control references found above (the glow colors live on, or next to, what they point at).
        {
            std::set<ZEntityType**> s_Done;
            uint32_t s_Followed = 0;
            for (const ZEntityRef& s_Target : s_Pending) {
                if (!s_Done.insert(s_Target.m_pObj).second) continue;
                ++s_Followed;
                s_Guarded(s_Target);
                if (s_Followed <= 3) {
                    std::string s_Names;
                    auto s_ListProps = [&]() {
                        ZEntityImpl* s_E = s_Target.GetEntity();
                        ZEntityType* s_T = s_E ? s_E->GetType() : nullptr;
                        if (!s_T || !s_T->m_pPropertyData) return;
                        uint32_t s_N = 0;
                        for (const SPropertyData& s_P : *s_T->m_pPropertyData) {
                            const SNamedPropertyInfo* s_NI = s_P.GetPropertyInfo();
                            if (s_NI && s_NI->m_pszPropertyName && ++s_N <= 120) s_Names += std::string(s_NI->m_pszPropertyName) + " ";
                        }
                    };
                    if (!SafeCall(s_ListProps)) s_Names += "(unreadable)";
                    s_LinkInfo += "  target " + std::to_string(s_Followed) + " properties: " + s_Names + "\n";
                }
            }
            std::string s_Types;
            for (const auto& s_It : s_InstinctTypes) s_Types += "size " + std::to_string(s_It.second.second) + " ";
            s_Detail = "m_pInstinctControl: found on " + std::to_string(s_InstinctTypes.size()) + " entity type(s) (" + s_Types +
                       "), referenced " + std::to_string(s_Pending.size()) + " time(s), " + std::to_string(s_Followed) +
                       " distinct target(s)\n" + s_LinkInfo + s_Detail;
        }

        ZEntitySceneContext* s_Ctx = Globals::Hitman5Module ? Globals::Hitman5Module->m_pEntitySceneContext : nullptr;
        if (s_Entities > 0) {
            s_Detail = "Config entity found through the activation hook; scene walk skipped.\n" + s_Detail;
        } else if (s_Ctx && s_Ctx->m_LoadingStage == ESceneLoadingStage::eLoading_ScenePlaying) {
            uint32_t s_BrickCount = 0;
            auto s_WalkAll = [&]() {
                s_WalkRoot(s_Ctx->m_pScene.m_entityRef, 0);
                for (const SBrickAllocationInfo& s_Brick : s_Ctx->m_aLoadedBricks) {
                    ++s_BrickCount;
                    s_WalkRoot(s_Brick.m_EntityRef, 0);
                }
            };
            if (!SafeCall(s_WalkAll)) ++s_Faults;
            s_Detail = std::string("Entity types with glow/outline/instinct color properties:\n") + (s_Cand.empty() ? "  (none)\n" : s_Cand) + "\n" + s_Detail;
            s_Detail = "Entities from entity manager: " + std::to_string(s_FromManager) +
                       "\nEntities from scene/bricks: " + std::to_string(s_Scanned - s_FromManager) +
                       "\nBricks walked: " + std::to_string(s_BrickCount) +
                       "\nNested templates walked: " + std::to_string(s_NestedRoots) +
                       "\nAccess faults survived: " + std::to_string(s_Faults) + "\n" + s_Detail;
        } else {
            s_Detail = std::string("Scene not in the playing state yet (stage ") +
                       (s_Ctx ? std::to_string(static_cast<int>(s_Ctx->m_LoadingStage)) : std::string("n/a")) + "): scene walk skipped.\n" + s_Detail;
        }
    } catch (...) {
        m_GlowSlots.clear();
    }

    // The game's own colors per category (from the first value found for each).
    for (int i = 0; i < kGlowCatCount; ++i) {
        SGlowSetting& s_G = m_Glow[i];
        bool s_GotFill = false, s_GotOutline = false;
        for (const SGlowSlot& s_Slot : m_GlowSlots) {
            if (s_Slot.Cat != i) continue;
            if (!s_Slot.Outline && !s_GotFill) { std::memcpy(s_G.VanFill, s_Slot.Vanilla, sizeof(s_G.VanFill)); s_GotFill = true; }
            if (s_Slot.Outline && !s_GotOutline) { std::memcpy(s_G.VanOutline, s_Slot.Vanilla, sizeof(s_G.VanOutline)); s_GotOutline = true; }
        }
        s_G.HasVanilla = s_GotFill && s_GotOutline;

        // First time we see the game's colors: start the custom colors from them, so "custom" looks like the game until changed.
        if (s_G.HasVanilla && !s_G.Seeded) {
            std::memcpy(s_G.Fill, s_G.VanFill, sizeof(s_G.Fill));
            std::memcpy(s_G.Outline, s_G.VanOutline, sizeof(s_G.Outline));
            for (int k = 0; k < 4; ++k) {
                s_G.Fill[k] = std::clamp(s_G.Fill[k], 0.0f, 1.0f);
                s_G.Outline[k] = std::clamp(s_G.Outline[k], 0.0f, 1.0f);
            }
            s_G.Seeded = true;
            m_GlowDirty = true;
        }
    }

    m_GlowScanned = s_Scanned;
    m_GlowEntities = s_Entities;
    m_GlowSkipped = s_Skipped;
    m_GlowFound = static_cast<uint32_t>(m_GlowSlots.size());
    m_GlowState = m_GlowSlots.empty() ? 1 : 2;
    m_GlowForce = true;

    {
        std::string s_Log = "Entities checked: " + std::to_string(s_Scanned) + "\nGlow config entities found: " + std::to_string(s_Entities) +
                            "\nColors found: " + std::to_string(m_GlowSlots.size()) + "\nSkipped: " + std::to_string(s_Skipped) + "\n";
        for (int i = 0; i < kGlowCatCount; ++i) {
            int s_N = 0;
            for (const SGlowSlot& s_Slot : m_GlowSlots) if (s_Slot.Cat == i) ++s_N;
            s_Log += std::string("  ") + kGlowCats[i].Key + ": " + std::to_string(s_N) + "\n";
        }
        s_Log += "Matching properties:\n" + s_Detail;
        GlowLog(s_Log);
    }
}

void BetterInstinct::WriteGlowSlots(bool p_NotifyGame) {
    for (SGlowSlot& s_Slot : m_GlowSlots) { // non-const: ZEntityRef::SetProperty is not a const member
        if (s_Slot.Cat < 0 || s_Slot.Cat >= kGlowCatCount) continue;
        const SGlowSetting& s_G = m_Glow[s_Slot.Cat];

        float s_V[4];
        if (s_G.Mode == 0) {
            std::memcpy(s_V, s_Slot.Vanilla, sizeof(s_V));
        } else {
            const float* s_Src = s_Slot.Outline ? s_G.Outline : s_G.Fill;
            const bool s_On = s_Slot.Outline ? s_G.OutlineOn : s_G.FillOn;
            std::memcpy(s_V, s_Src, sizeof(s_V));
            if (!s_On) s_V[3] = 0.0f; // hidden = fully transparent
        }

        if (s_Slot.Ptr) {
            if (!CanWriteMemory(s_Slot.Ptr, s_Slot.Size)) continue;
            std::memcpy(s_Slot.Ptr, s_V, s_Slot.Size); // size 12 (RGB only) just drops the alpha
        }

        // Writing the memory alone isn't enough when the game only reads these colors when they change:
        // set the property the normal way too, so the game's change handlers run.
        if (p_NotifyGame) {
            try {
                if (s_Slot.Size == 16) {
                    SColorRGBA s_Color;
                    s_Color.r = s_V[0]; s_Color.g = s_V[1]; s_Color.b = s_V[2]; s_Color.a = s_V[3];
                    s_Slot.Ref.SetProperty<SColorRGBA>(s_Slot.PropId, s_Color, true);
                } else {
                    SColorRGB s_Color;
                    s_Color.r = s_V[0]; s_Color.g = s_V[1]; s_Color.b = s_V[2];
                    s_Slot.Ref.SetProperty<SColorRGB>(s_Slot.PropId, s_Color, true);
                }
            } catch (...) {
            }
        }
    }
}

void BetterInstinct::RestoreGlowColors() {
    for (SGlowSlot& s_Slot : m_GlowSlots) {
        if (s_Slot.Ptr) {
            if (!CanWriteMemory(s_Slot.Ptr, s_Slot.Size)) continue;
            std::memcpy(s_Slot.Ptr, s_Slot.Vanilla, s_Slot.Size);
        } else {
            try { // getter/setter property: put the game's color back through the setter
                if (s_Slot.Size == 16) {
                    SColorRGBA s_Color;
                    s_Color.r = s_Slot.Vanilla[0]; s_Color.g = s_Slot.Vanilla[1]; s_Color.b = s_Slot.Vanilla[2]; s_Color.a = s_Slot.Vanilla[3];
                    s_Slot.Ref.SetProperty<SColorRGBA>(s_Slot.PropId, s_Color, true);
                } else {
                    SColorRGB s_Color;
                    s_Color.r = s_Slot.Vanilla[0]; s_Color.g = s_Slot.Vanilla[1]; s_Color.b = s_Slot.Vanilla[2];
                    s_Slot.Ref.SetProperty<SColorRGB>(s_Slot.PropId, s_Color, true);
                }
            } catch (...) {
            }
        }
    }
}

void BetterInstinct::UpdateGlowColors(float p_Dt) {
    if (m_GlowRescanRequest.exchange(false)) {
        m_GlowNeedScan = true;
        m_GlowKeepSnap = true; // same mission: keep the game's colors we already remembered
        m_GlowWait = 0.0f;
    }

    if (m_GlowNeedScan) {
        m_GlowWait -= p_Dt;
        if (m_GlowWait <= 0.0f) {
            ScanGlowSlots(m_GlowKeepSnap);
            m_GlowKeepSnap = false;
            if (m_GlowSlots.empty()) {
                m_GlowWait = 20.0f; // nothing yet (the mission is still loading): try again later (a scene scan is not cheap)
            } else {
                m_GlowNeedScan = false;
            }
        }
    }
    if (m_GlowSlots.empty()) return;

    bool s_AnyCustom = false;
    for (const SGlowSetting& s_G : m_Glow) {
        if (s_G.Mode != 0) s_AnyCustom = true;
    }

    m_GlowTimer += p_Dt;
    const uint32_t s_Sig = GlowSignature();
    if (m_GlowForce || s_Sig != m_GlowAppliedSig || (s_AnyCustom && m_GlowTimer >= 1.0f)) {
        // While custom colors are on they are written again about once a second, in case the game resets them.
        WriteGlowSlots(m_GlowForce || s_Sig != m_GlowAppliedSig);
        m_GlowAppliedSig = s_Sig;
        m_GlowForce = false;
        m_GlowTimer = 0.0f;
    }
}

// ---------------------------------------------------------------------------
// Game loop
// ---------------------------------------------------------------------------

void BetterInstinct::OnFrameUpdate(const SGameUpdateEvent& p_UpdateEvent) {
    if (!Globals::GameTimeManager) {
        return;
    }

    // Real (unscaled) frame delta, so neither the meter nor the blend depend on the slowdown itself.
    using clock = std::chrono::steady_clock;
    const auto s_Now = clock::now();
    const float s_RawGap = std::chrono::duration<float>(s_Now - m_LastFrame).count();
    const float s_Dt = std::min(s_RawGap, 0.1f);
    m_LastFrame = s_Now;
    if (s_RawGap > 1.5f) { // the game loop stood still for a while (new mission, restart...): old entity pointers can't be trusted
        m_GlowSlots.clear();
        m_GlowNeedScan = true;
        m_GlowKeepSnap = false;
        m_GlowWait = 2.0f;
        m_GlowState = 0;
    }
    m_LastUpdateTicks.store(s_Now.time_since_epoch().count());
    ++m_FrameCount;

    // Only run the Instinct logic while Agent 47 actually exists, so nothing happens in the main menu / hub screens.
    bool s_HasPlayer = false;
    if (auto s_LocalHitman = SDK()->GetLocalPlayer()) {
        s_HasPlayer = true;
    }
    m_HasPlayer = s_HasPlayer;

    if (!s_HasPlayer) {
        m_NoPlayerTime += s_Dt;
        m_Active.store(false);
        m_InputBlock = false;
        g_Block = false;
        m_PrevDown = false;
        UpdateTint(s_Dt, false); // fade out and restore the tint when there is no player

        if (m_NoPlayerTime > 1.0f) { // really out of the mission (not just a short gap): start the next one fresh
            m_GlowSlots.clear(); // the mission's entities are gone: find them again in the next one
            m_GlowNeedScan = true;
            m_GlowKeepSnap = false;
            m_GlowWait = 3.0f;
            m_GlowState = 0;
            {
                std::lock_guard<std::mutex> s_ZoomLock(m_ZoomMutex);
                m_ZoomProfiles.clear(); // the mission's entities are gone
            }
            m_ZoomFound.store(0);
            m_ZoomTarget = m_ZoomCur = m_ZoomWritten = 1.0f;
            g_ZoomWanted.store(false);
            m_Meter = 1.0f;
            m_Locked = false;
            m_Blocked = false;
            m_NeedRelease = false;
            m_ToggleState = false;
            m_RechargeWait = 0.0f;
            m_HoldVisible = 0.0f;

            if (m_Touched) {
                Globals::GameTimeManager->m_fGameTimeMultiplier = 1.0f;
                m_CurrentScale = 1.0f;
                m_Touched = false;
            }
        }
        return;
    }
    m_NoPlayerTime = 0.0f;

    UpdateGlowColors(s_Dt);
    UpdateZoom(s_Dt);

    // Only react to keys when the game window is the active window.
    DWORD s_ForegroundPid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &s_ForegroundPid);
    const bool s_GameFocused = s_ForegroundPid == GetCurrentProcessId() && !m_Binding;
    const bool s_KeyDown = s_GameFocused && InstinctKeyDown();
    const bool s_PressEdge = s_KeyDown && !m_PrevDown;

    g_Key = m_InstinctKey;

    // Is Instinct usable right now? Not if the meter ran dry (until it recovers) or is below the start minimum.
    const bool s_WasActive = m_Active.load();
    m_Blocked = m_Locked || (!s_WasActive && m_Meter < kMinStart);

    // Block the key at the input level while unusable, so the game can't switch its own Instinct on either.
    g_Block = m_BlockEnabled && m_Blocked;
    // Same at the game level: the game is told the Instinct action is not pressed.
    m_InputBlock = (m_BlockEnabled && m_Blocked) || m_ForceBlockTest;

    // Safety net that doesn't depend on knowing the game's action name: while Instinct is unusable and the
    // player presses/holds the key, undo it the same way as when the meter runs dry (inject a key release).
    //  - Hold mode: release every frame while the key is held (also beats key auto-repeat).
    //  - Toggle mode: tap once on the press, which flips the game's Instinct straight back off.
    if (m_BlockEnabled && m_Blocked && s_KeyDown) {
        if (m_ToggleMode) {
            if (s_PressEdge) ForceReleaseInstinct();
        } else {
            ForceReleaseInstinct();
        }
    }

    // A key press that happened while blocked (or that we force-released) must be released before Instinct can start again.
    if (!s_KeyDown) {
        m_NeedRelease = false;
    } else if (m_Blocked) {
        m_NeedRelease = true;
    }

    // Does the player want Instinct right now?
    bool s_Want;
    if (m_ToggleMode) {
        if (s_KeyDown && !m_PrevDown && !m_Blocked) {
            m_ToggleState = !m_ToggleState;
        }
        if (m_Blocked) {
            m_ToggleState = false;
        }
        s_Want = m_ToggleState;
    } else {
        m_ToggleState = false;
        s_Want = s_KeyDown && !m_NeedRelease && !m_Blocked;
    }
    m_PrevDown = s_KeyDown;

    // Ran dry while in use: lock Instinct and make the game drop it too.
    if (s_Want && m_Meter <= 0.0f) {
        m_Locked = true;
        m_Blocked = true;
        m_NeedRelease = true;
        m_ToggleState = false;
        s_Want = false;
        if (m_BlockEnabled) {
            g_Block = true;
            m_InputBlock = true; // game-level block takes effect immediately
            ForceReleaseInstinct();
        }
    }

    m_Active.store(s_Want);
    UpdateTint(s_Dt, s_Want);

    // Meter: drains while active, recharges (after a short delay) otherwise.
    if (s_Want) {
        m_Meter -= s_Dt / m_DrainSeconds;
        m_RechargeWait = m_RechargeDelay;
    } else if (m_RechargeWait > 0.0f) {
        m_RechargeWait -= s_Dt;
    } else {
        m_Meter += s_Dt / m_RechargeSeconds;
    }
    m_Meter = std::clamp(m_Meter, 0.0f, 1.0f);

    if (m_Locked && m_Meter >= kUnlockLevel) {
        m_Locked = false;
    }

    if (s_Want || m_Meter < 0.999f) {
        m_HoldVisible = 1.5f;
    } else {
        m_HoldVisible = std::max(0.0f, m_HoldVisible - s_Dt);
    }

    // Time scale.
    const float s_Target = s_Want ? m_FocusScale : 1.0f;
    const float s_T = std::clamp(m_BlendSpeed * s_Dt, 0.0f, 1.0f);
    m_CurrentScale += (s_Target - m_CurrentScale) * s_T;

    if (s_Want || std::abs(m_CurrentScale - 1.0f) > 0.002f) {
        Globals::GameTimeManager->m_fGameTimeMultiplier = m_CurrentScale;
        m_Touched = true;
    } else if (m_Touched) {
        // Back to normal: hand control of the multiplier back to the game.
        Globals::GameTimeManager->m_fGameTimeMultiplier = 1.0f;
        m_CurrentScale = 1.0f;
        m_Touched = false;
    }
}

// ---------------------------------------------------------------------------
// Instinct color (replaces the grey Instinct color-grading LUT)
// ---------------------------------------------------------------------------
//
// The grey Instinct look is not a post-filter color value: it is a 64x64 color-grading lookup table (LUT) texture,
// resource 008C1F5C7305A978 (the same texture the "Alternate Instinct Overlay" mod replaces). Every texel says
// "input color -> output color", and the game blends it in while Instinct is on. So we:
//   1. find that texture in the game's resource container and its ID3D12Resource,
//   2. read the vanilla (grey) LUT back from the GPU once (and cache it next to the config file),
//   3. build a recolored copy: monochrome in the player's color, mixed with the vanilla LUT by "Strength",
//   4. write the copy into the texture's GPU memory. It only changes when a setting changes.
// Because it is the scene's own color grading, the UI, menus and map are never affected, and since the vanilla grey
// is replaced (not mixed with anything), only the chosen color remains.

namespace {

constexpr uint64_t kInstinctLutId = 0x008C1F5C7305A978ULL;
constexpr UINT kLutDim = 64;

bool LutSafeReadPtr(const void* p_Addr, void** p_Out) {
    __try {
        *p_Out = *reinterpret_cast<void* const*>(p_Addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// True when the object's vtable lives inside a d3d12 module (cheap check before calling QueryInterface on it).
bool LutIsD3DObject(void* p_Obj) {
    void* s_Vtbl = nullptr;
    if (!LutSafeReadPtr(p_Obj, &s_Vtbl) || !s_Vtbl) return false;

    HMODULE s_Module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(s_Vtbl), &s_Module) || !s_Module) {
        return false;
    }

    wchar_t s_Name[MAX_PATH] = {};
    if (!GetModuleFileNameW(s_Module, s_Name, MAX_PATH)) return false;
    for (wchar_t* c = s_Name; *c; ++c) *c = static_cast<wchar_t>(std::towlower(*c));
    return std::wcsstr(s_Name, L"d3d12") != nullptr;
}

ID3D12Resource* LutSafeQuery(void* p_Obj) {
    __try {
        ID3D12Resource* s_Res = nullptr;
        if (SUCCEEDED(reinterpret_cast<IUnknown*>(p_Obj)->QueryInterface(__uuidof(ID3D12Resource),
                                                                         reinterpret_cast<void**>(&s_Res)))) {
            return s_Res;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return nullptr;
}

// Returns an AddRef'd ID3D12Resource when p_Cand is the 64x64 2D texture we are looking for.
ID3D12Resource* LutTryCandidate(void* p_Cand) {
    if (!p_Cand || (reinterpret_cast<uintptr_t>(p_Cand) & 7) || reinterpret_cast<uintptr_t>(p_Cand) < 0x10000) return nullptr;
    if (!LutIsD3DObject(p_Cand)) return nullptr;

    ID3D12Resource* s_Res = LutSafeQuery(p_Cand);
    if (!s_Res) return nullptr;

    const D3D12_RESOURCE_DESC s_Desc = s_Res->GetDesc();
    if (s_Desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && s_Desc.Width == kLutDim && s_Desc.Height == kLutDim &&
        s_Desc.DepthOrArraySize == 1) {
        return s_Res;
    }

    s_Res->Release();
    return nullptr;
}

// The loaded TEXT resource wraps the GPU texture somewhere inside it; look through the object (and one level of
// pointers inside it) without needing to know the exact layout.
ID3D12Resource* LutProbe(void* p_Data, std::string& p_How) {
    if (!p_Data) return nullptr;

    if (ID3D12Resource* s_Res = LutTryCandidate(p_Data)) {
        p_How = "resource data is the texture";
        return s_Res;
    }

    char s_Buf[96];
    for (size_t s_Off = 0; s_Off < 0x100; s_Off += 8) {
        void* s_Q = nullptr;
        if (!LutSafeReadPtr(static_cast<char*>(p_Data) + s_Off, &s_Q)) break;
        if (ID3D12Resource* s_Res = LutTryCandidate(s_Q)) {
            std::snprintf(s_Buf, sizeof(s_Buf), "texture at resource data + 0x%zX", s_Off);
            p_How = s_Buf;
            return s_Res;
        }
    }

    for (size_t s_Off = 0; s_Off < 0x100; s_Off += 8) {
        void* s_Q = nullptr;
        if (!LutSafeReadPtr(static_cast<char*>(p_Data) + s_Off, &s_Q)) break;
        if (!s_Q || (reinterpret_cast<uintptr_t>(s_Q) & 7) || reinterpret_cast<uintptr_t>(s_Q) < 0x10000) continue;

        for (size_t s_Off2 = 0; s_Off2 < 0x100; s_Off2 += 8) {
            void* s_Q2 = nullptr;
            if (!LutSafeReadPtr(static_cast<char*>(s_Q) + s_Off2, &s_Q2)) break;
            if (ID3D12Resource* s_Res = LutTryCandidate(s_Q2)) {
                std::snprintf(s_Buf, sizeof(s_Buf), "texture at resource data + 0x%zX -> + 0x%zX", s_Off, s_Off2);
                p_How = s_Buf;
                return s_Res;
            }
        }
    }

    return nullptr;
}

} // namespace

// GPU-side state for the LUT texture (kept out of the header so it doesn't need the D3D12 headers).
struct SLutGpu {
    Microsoft::WRL::ComPtr<ID3D12Resource> m_Res;
    Microsoft::WRL::ComPtr<ID3D12Device> m_Dev;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_Queue;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_Alloc;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_List;
    Microsoft::WRL::ComPtr<ID3D12Fence> m_Fence;
    HANDLE m_Event = nullptr;
    UINT64 m_FenceValue = 0;
    D3D12_RESOURCE_DESC m_Desc{};
    uint32_t m_Index = 0;      // index in the resource container
    void* m_Data = nullptr;    // resourceData pointer at the time we found it (changes when the game reloads it)
    std::vector<uint8_t> m_Base; // vanilla LUT, mip 0, always RGBA order
    bool m_Bgra = false;
    bool m_NoAlpha = false;
    std::string m_How;

    ~SLutGpu() {
        if (m_Event) CloseHandle(m_Event);
    }
};

namespace {

bool LutFormatInfo(DXGI_FORMAT p_Format, bool& p_Bgra, bool& p_NoAlpha) {
    p_NoAlpha = false;
    switch (p_Format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        p_Bgra = false;
        return true;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        p_Bgra = true;
        return true;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        p_Bgra = true;
        p_NoAlpha = true;
        return true;
    default:
        return false;
    }
}

bool LutBegin(SLutGpu& p_Gpu) {
    return SUCCEEDED(p_Gpu.m_Alloc->Reset()) && SUCCEEDED(p_Gpu.m_List->Reset(p_Gpu.m_Alloc.Get(), nullptr));
}

// Submits the recorded list on our own queue and waits for it (the LUT is tiny, so this is well under a millisecond).
bool LutRun(SLutGpu& p_Gpu) {
    if (FAILED(p_Gpu.m_List->Close())) return false;

    ID3D12CommandList* s_Lists[] = {p_Gpu.m_List.Get()};
    p_Gpu.m_Queue->ExecuteCommandLists(1, s_Lists);

    const UINT64 s_Value = ++p_Gpu.m_FenceValue;
    if (FAILED(p_Gpu.m_Queue->Signal(p_Gpu.m_Fence.Get(), s_Value))) return false;

    if (p_Gpu.m_Fence->GetCompletedValue() < s_Value) {
        if (FAILED(p_Gpu.m_Fence->SetEventOnCompletion(s_Value, p_Gpu.m_Event))) return false;
        if (WaitForSingleObject(p_Gpu.m_Event, 3000) != WAIT_OBJECT_0) return false;
    }
    return true;
}

D3D12_RESOURCE_BARRIER LutBarrier(ID3D12Resource* p_Res, D3D12_RESOURCE_STATES p_From, D3D12_RESOURCE_STATES p_To) {
    D3D12_RESOURCE_BARRIER s_Barrier{};
    s_Barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    s_Barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    s_Barrier.Transition.pResource = p_Res;
    s_Barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    s_Barrier.Transition.StateBefore = p_From;
    s_Barrier.Transition.StateAfter = p_To;
    return s_Barrier;
}

// State the game leaves the texture in. 0 = shader resource (normal), 1 = common (if the first doesn't work).
D3D12_RESOURCE_STATES LutRestState(int p_Mode) {
    return p_Mode == 0 ? D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_COMMON;
}

D3D12_RESOURCE_DESC LutBufferDesc(UINT64 p_Size) {
    D3D12_RESOURCE_DESC s_Desc{};
    s_Desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    s_Desc.Width = p_Size;
    s_Desc.Height = 1;
    s_Desc.DepthOrArraySize = 1;
    s_Desc.MipLevels = 1;
    s_Desc.Format = DXGI_FORMAT_UNKNOWN;
    s_Desc.SampleDesc.Count = 1;
    s_Desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return s_Desc;
}

bool LutInit(SLutGpu& p_Gpu, ID3D12Resource* p_Res, std::string& p_Err) {
    p_Gpu.m_Res.Attach(p_Res); // takes the reference LutTryCandidate gave us
    p_Gpu.m_Desc = p_Res->GetDesc();

    if (!LutFormatInfo(p_Gpu.m_Desc.Format, p_Gpu.m_Bgra, p_Gpu.m_NoAlpha)) {
        p_Err = "the LUT texture has an unsupported pixel format (DXGI " + std::to_string(static_cast<int>(p_Gpu.m_Desc.Format)) + ")";
        return false;
    }

    if (FAILED(p_Res->GetDevice(IID_PPV_ARGS(&p_Gpu.m_Dev))) || !p_Gpu.m_Dev) {
        p_Err = "could not get the D3D12 device of the LUT texture";
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC s_QueueDesc{};
    s_QueueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(p_Gpu.m_Dev->CreateCommandQueue(&s_QueueDesc, IID_PPV_ARGS(&p_Gpu.m_Queue))) ||
        FAILED(p_Gpu.m_Dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&p_Gpu.m_Alloc))) ||
        FAILED(p_Gpu.m_Dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, p_Gpu.m_Alloc.Get(), nullptr,
                                              IID_PPV_ARGS(&p_Gpu.m_List))) ||
        FAILED(p_Gpu.m_Dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&p_Gpu.m_Fence)))) {
        p_Err = "could not create the D3D12 copy objects";
        return false;
    }
    p_Gpu.m_List->Close();

    p_Gpu.m_Event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!p_Gpu.m_Event) {
        p_Err = "could not create a wait event";
        return false;
    }
    return true;
}

// Reads mip 0 of the texture into m_Base (RGBA order).
bool LutReadback(SLutGpu& p_Gpu, int p_State, std::string& p_Err) {
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT s_Fp{};
    UINT s_Rows = 0;
    UINT64 s_RowSize = 0, s_Total = 0;
    p_Gpu.m_Dev->GetCopyableFootprints(&p_Gpu.m_Desc, 0, 1, 0, &s_Fp, &s_Rows, &s_RowSize, &s_Total);
    if (s_Total == 0) {
        p_Err = "texture footprint is empty";
        return false;
    }

    D3D12_HEAP_PROPERTIES s_Heap{};
    s_Heap.Type = D3D12_HEAP_TYPE_READBACK;
    s_Heap.CreationNodeMask = 1;
    s_Heap.VisibleNodeMask = 1;
    const D3D12_RESOURCE_DESC s_BufDesc = LutBufferDesc(s_Total);

    Microsoft::WRL::ComPtr<ID3D12Resource> s_Readback;
    if (FAILED(p_Gpu.m_Dev->CreateCommittedResource(&s_Heap, D3D12_HEAP_FLAG_NONE, &s_BufDesc, D3D12_RESOURCE_STATE_COPY_DEST,
                                                    nullptr, IID_PPV_ARGS(&s_Readback)))) {
        p_Err = "could not create the readback buffer";
        return false;
    }

    const D3D12_RESOURCE_STATES s_Rest = LutRestState(p_State);
    if (!LutBegin(p_Gpu)) {
        p_Err = "could not start the copy command list";
        return false;
    }

    D3D12_RESOURCE_BARRIER s_Barrier = LutBarrier(p_Gpu.m_Res.Get(), s_Rest, D3D12_RESOURCE_STATE_COPY_SOURCE);
    p_Gpu.m_List->ResourceBarrier(1, &s_Barrier);

    D3D12_TEXTURE_COPY_LOCATION s_Dst{};
    s_Dst.pResource = s_Readback.Get();
    s_Dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    s_Dst.PlacedFootprint = s_Fp;
    D3D12_TEXTURE_COPY_LOCATION s_Src{};
    s_Src.pResource = p_Gpu.m_Res.Get();
    s_Src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    s_Src.SubresourceIndex = 0;
    p_Gpu.m_List->CopyTextureRegion(&s_Dst, 0, 0, 0, &s_Src, nullptr);

    s_Barrier = LutBarrier(p_Gpu.m_Res.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, s_Rest);
    p_Gpu.m_List->ResourceBarrier(1, &s_Barrier);

    if (!LutRun(p_Gpu)) {
        p_Err = "the GPU copy did not finish";
        return false;
    }

    void* s_Mapped = nullptr;
    const D3D12_RANGE s_Range{0, static_cast<SIZE_T>(s_Total)};
    if (FAILED(s_Readback->Map(0, &s_Range, &s_Mapped)) || !s_Mapped) {
        p_Err = "could not read the readback buffer";
        return false;
    }

    const uint32_t s_W = static_cast<uint32_t>(p_Gpu.m_Desc.Width);
    const uint32_t s_H = p_Gpu.m_Desc.Height;
    p_Gpu.m_Base.assign(static_cast<size_t>(s_W) * s_H * 4, 255);
    for (uint32_t y = 0; y < s_H; ++y) {
        const uint8_t* s_Row = static_cast<const uint8_t*>(s_Mapped) + s_Fp.Offset + static_cast<size_t>(y) * s_Fp.Footprint.RowPitch;
        uint8_t* s_Out = &p_Gpu.m_Base[static_cast<size_t>(y) * s_W * 4];
        for (uint32_t x = 0; x < s_W; ++x) {
            const uint8_t* s_Px = s_Row + x * 4;
            s_Out[x * 4 + 0] = p_Gpu.m_Bgra ? s_Px[2] : s_Px[0];
            s_Out[x * 4 + 1] = s_Px[1];
            s_Out[x * 4 + 2] = p_Gpu.m_Bgra ? s_Px[0] : s_Px[2];
            s_Out[x * 4 + 3] = p_Gpu.m_NoAlpha ? 255 : s_Px[3];
        }
    }

    const D3D12_RANGE s_NoWrite{0, 0};
    s_Readback->Unmap(0, &s_NoWrite);
    return true;
}

// Writes an RGBA image (mip 0) into the texture; lower mips (if the texture has any) are box-filtered from it.
bool LutWrite(SLutGpu& p_Gpu, int p_State, const std::vector<uint8_t>& p_Rgba, std::string& p_Err) {
    const UINT s_Mips = p_Gpu.m_Desc.MipLevels ? p_Gpu.m_Desc.MipLevels : 1;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> s_Fp(s_Mips);
    std::vector<UINT> s_Rows(s_Mips);
    std::vector<UINT64> s_RowSize(s_Mips);
    UINT64 s_Total = 0;
    p_Gpu.m_Dev->GetCopyableFootprints(&p_Gpu.m_Desc, 0, s_Mips, 0, s_Fp.data(), s_Rows.data(), s_RowSize.data(), &s_Total);
    if (s_Total == 0) {
        p_Err = "texture footprint is empty";
        return false;
    }

    D3D12_HEAP_PROPERTIES s_Heap{};
    s_Heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    s_Heap.CreationNodeMask = 1;
    s_Heap.VisibleNodeMask = 1;
    const D3D12_RESOURCE_DESC s_BufDesc = LutBufferDesc(s_Total);

    Microsoft::WRL::ComPtr<ID3D12Resource> s_Upload;
    if (FAILED(p_Gpu.m_Dev->CreateCommittedResource(&s_Heap, D3D12_HEAP_FLAG_NONE, &s_BufDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                    nullptr, IID_PPV_ARGS(&s_Upload)))) {
        p_Err = "could not create the upload buffer";
        return false;
    }

    void* s_Mapped = nullptr;
    const D3D12_RANGE s_NoRead{0, 0};
    if (FAILED(s_Upload->Map(0, &s_NoRead, &s_Mapped)) || !s_Mapped) {
        p_Err = "could not map the upload buffer";
        return false;
    }

    uint32_t s_Mw = static_cast<uint32_t>(p_Gpu.m_Desc.Width);
    uint32_t s_Mh = p_Gpu.m_Desc.Height;
    std::vector<uint8_t> s_Cur = p_Rgba;
    for (UINT m = 0; m < s_Mips; ++m) {
        if (m > 0) {
            const uint32_t s_Pw = s_Mw, s_Ph = s_Mh;
            s_Mw = std::max(1u, s_Pw / 2);
            s_Mh = std::max(1u, s_Ph / 2);
            std::vector<uint8_t> s_Next(static_cast<size_t>(s_Mw) * s_Mh * 4);
            for (uint32_t y = 0; y < s_Mh; ++y) {
                for (uint32_t x = 0; x < s_Mw; ++x) {
                    const uint32_t s_X0 = std::min(x * 2, s_Pw - 1), s_X1 = std::min(x * 2 + 1, s_Pw - 1);
                    const uint32_t s_Y0 = std::min(y * 2, s_Ph - 1), s_Y1 = std::min(y * 2 + 1, s_Ph - 1);
                    for (int c = 0; c < 4; ++c) {
                        const uint32_t s_Sum = s_Cur[(static_cast<size_t>(s_Y0) * s_Pw + s_X0) * 4 + c] +
                                               s_Cur[(static_cast<size_t>(s_Y0) * s_Pw + s_X1) * 4 + c] +
                                               s_Cur[(static_cast<size_t>(s_Y1) * s_Pw + s_X0) * 4 + c] +
                                               s_Cur[(static_cast<size_t>(s_Y1) * s_Pw + s_X1) * 4 + c];
                        s_Next[(static_cast<size_t>(y) * s_Mw + x) * 4 + c] = static_cast<uint8_t>((s_Sum + 2) / 4);
                    }
                }
            }
            s_Cur = std::move(s_Next);
        }

        for (uint32_t y = 0; y < s_Mh; ++y) {
            uint8_t* s_Row = static_cast<uint8_t*>(s_Mapped) + s_Fp[m].Offset + static_cast<size_t>(y) * s_Fp[m].Footprint.RowPitch;
            for (uint32_t x = 0; x < s_Mw; ++x) {
                const uint8_t* s_Px = &s_Cur[(static_cast<size_t>(y) * s_Mw + x) * 4];
                s_Row[x * 4 + 0] = p_Gpu.m_Bgra ? s_Px[2] : s_Px[0];
                s_Row[x * 4 + 1] = s_Px[1];
                s_Row[x * 4 + 2] = p_Gpu.m_Bgra ? s_Px[0] : s_Px[2];
                s_Row[x * 4 + 3] = p_Gpu.m_NoAlpha ? 255 : s_Px[3];
            }
        }
    }
    s_Upload->Unmap(0, nullptr);

    const D3D12_RESOURCE_STATES s_Rest = LutRestState(p_State);
    if (!LutBegin(p_Gpu)) {
        p_Err = "could not start the copy command list";
        return false;
    }

    D3D12_RESOURCE_BARRIER s_Barrier = LutBarrier(p_Gpu.m_Res.Get(), s_Rest, D3D12_RESOURCE_STATE_COPY_DEST);
    p_Gpu.m_List->ResourceBarrier(1, &s_Barrier);

    for (UINT m = 0; m < s_Mips; ++m) {
        D3D12_TEXTURE_COPY_LOCATION s_Dst{};
        s_Dst.pResource = p_Gpu.m_Res.Get();
        s_Dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        s_Dst.SubresourceIndex = m;
        D3D12_TEXTURE_COPY_LOCATION s_Src{};
        s_Src.pResource = s_Upload.Get();
        s_Src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        s_Src.PlacedFootprint = s_Fp[m];
        p_Gpu.m_List->CopyTextureRegion(&s_Dst, 0, 0, 0, &s_Src, nullptr);
    }

    s_Barrier = LutBarrier(p_Gpu.m_Res.Get(), D3D12_RESOURCE_STATE_COPY_DEST, s_Rest);
    p_Gpu.m_List->ResourceBarrier(1, &s_Barrier);

    if (!LutRun(p_Gpu)) {
        p_Err = "the GPU copy did not finish";
        return false;
    }
    return true;
}

// Monochrome copy of the vanilla LUT in the chosen color. Strength 1 replaces the vanilla look completely.
std::vector<uint8_t> LutRecolor(const std::vector<uint8_t>& p_Base, const float* p_Color, float p_Strength, bool p_Keep, float p_Bright) {
    const float s_Max = std::max({p_Color[0], p_Color[1], p_Color[2], 0.001f});
    const float s_C[3] = {p_Color[0] / s_Max, p_Color[1] / s_Max, p_Color[2] / s_Max};
    const float s_LumC = 0.2126f * s_C[0] + 0.7152f * s_C[1] + 0.0722f * s_C[2];
    // "Keep brightness": dark colors (blue, red) would otherwise make a much darker picture than vanilla Instinct.
    const float s_Boost = p_Bright * (p_Keep ? 1.0f / std::max(s_LumC, 0.30f) : 1.0f);

    std::vector<uint8_t> s_Out(p_Base.size());
    for (size_t i = 0; i + 3 < p_Base.size(); i += 4) {
        const float s_R = p_Base[i + 0] / 255.0f, s_G = p_Base[i + 1] / 255.0f, s_B = p_Base[i + 2] / 255.0f;
        const float s_L = 0.2126f * s_R + 0.7152f * s_G + 0.0722f * s_B;
        const float s_In[3] = {s_R, s_G, s_B};
        for (int c = 0; c < 3; ++c) {
            const float s_Mono = std::clamp(s_L * s_C[c] * s_Boost, 0.0f, 1.0f);
            const float s_V = s_In[c] + (s_Mono - s_In[c]) * p_Strength;
            s_Out[i + c] = static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(s_V * 255.0f)), 0, 255));
        }
        s_Out[i + 3] = p_Base[i + 3];
    }
    return s_Out;
}

// "No Instinct color": an identity LUT (every color maps to itself), so Instinct's grading changes nothing on screen.
// The layout of the game's LUT is not known in advance, so it is worked out from the vanilla (grey) data itself: the
// vanilla LUT outputs a luminance of its input, so the right layout is the one where "luminance of the input color the
// texel stands for" matches the vanilla texel. If no layout matches well, nothing is written (the picture stays vanilla).
std::vector<uint8_t> LutIdentity(const std::vector<uint8_t>& p_Base, int p_W, int p_H, std::string& p_Err) {
    int s_N = 0;
    for (int n = 2; n <= 64; ++n) if (n * n * n == p_W * p_H) s_N = n;
    if (s_N == 0 || p_W % s_N != 0 || p_H % s_N != 0 || (p_W / s_N) * (p_H / s_N) != s_N ||
        p_Base.size() < static_cast<size_t>(p_W) * p_H * 4) {
        p_Err = "the Instinct texture is not a 3D color LUT layout I recognise";
        return {};
    }

    const int s_TilesX = p_W / s_N;
    const float s_Den = static_cast<float>(s_N - 1);
    const int s_Count = p_W * p_H;

    std::vector<float> s_BaseLum(s_Count);
    double s_Mean = 0.0;
    for (int i = 0; i < s_Count; ++i) {
        const float s_L = 0.2126f * p_Base[i * 4 + 0] + 0.7152f * p_Base[i * 4 + 1] + 0.0722f * p_Base[i * 4 + 2];
        s_BaseLum[i] = s_L / 255.0f;
        s_Mean += s_BaseLum[i];
    }
    s_Mean /= s_Count;
    double s_VarBase = 0.0;
    for (int i = 0; i < s_Count; ++i) s_VarBase += (s_BaseLum[i] - s_Mean) * (s_BaseLum[i] - s_Mean);
    if (s_VarBase < 1e-6) {
        p_Err = "the vanilla Instinct texture has no brightness variation to learn its layout from";
        return {};
    }

    static const int s_Perms[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
    double s_BestCorr = -2.0;
    int s_BestPerm = 0, s_BestFlip = 0;

    auto s_InputAt = [&](int p_Texel, int p_Perm, int p_Flip, float* p_Rgb) {
        const int x = p_Texel % p_W, y = p_Texel / p_W;
        const int s_Tile = (y / s_N) * s_TilesX + (x / s_N);
        const int s_Axes[3] = {x % s_N, y % s_N, s_Tile}; // 0 = across the tile, 1 = down the tile, 2 = which tile
        for (int a = 0; a < 3; ++a) {
            float s_V = static_cast<float>(s_Axes[a]) / s_Den;
            if (p_Flip & (1 << a)) s_V = 1.0f - s_V;
            p_Rgb[s_Perms[p_Perm][a]] = s_V;
        }
    };

    for (int s_P = 0; s_P < 6; ++s_P) {
        for (int s_F = 0; s_F < 8; ++s_F) {
            double s_MeanIn = 0.0;
            std::vector<float> s_InLum(s_Count);
            for (int i = 0; i < s_Count; ++i) {
                float s_Rgb[3];
                s_InputAt(i, s_P, s_F, s_Rgb);
                s_InLum[i] = 0.2126f * s_Rgb[0] + 0.7152f * s_Rgb[1] + 0.0722f * s_Rgb[2];
                s_MeanIn += s_InLum[i];
            }
            s_MeanIn /= s_Count;
            double s_Cov = 0.0, s_VarIn = 0.0;
            for (int i = 0; i < s_Count; ++i) {
                s_Cov += (s_InLum[i] - s_MeanIn) * (s_BaseLum[i] - s_Mean);
                s_VarIn += (s_InLum[i] - s_MeanIn) * (s_InLum[i] - s_MeanIn);
            }
            const double s_Corr = s_VarIn > 1e-9 ? s_Cov / std::sqrt(s_VarIn * s_VarBase) : -2.0;
            if (s_Corr > s_BestCorr) { s_BestCorr = s_Corr; s_BestPerm = s_P; s_BestFlip = s_F; }
        }
    }

    if (s_BestCorr < 0.97) {
        char s_Buf[128];
        std::snprintf(s_Buf, sizeof(s_Buf), "could not work out the Instinct texture layout (best match %.2f)", s_BestCorr);
        p_Err = s_Buf;
        return {};
    }

    std::vector<uint8_t> s_Out(p_Base.size());
    for (int i = 0; i < s_Count; ++i) {
        float s_Rgb[3];
        s_InputAt(i, s_BestPerm, s_BestFlip, s_Rgb);
        for (int c = 0; c < 3; ++c) {
            s_Out[i * 4 + c] = static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(s_Rgb[c] * 255.0f)), 0, 255));
        }
        s_Out[i * 4 + 3] = p_Base[i * 4 + 3];
    }
    return s_Out;
}

fs::path LutCachePath() {
    const std::wstring s_Dir = ConfigDir();
    return s_Dir.empty() ? fs::path() : fs::path(s_Dir) / L"instinct_lut_base.bin";
}

} // namespace

void BetterInstinct::SetLutStatus(int p_State, const std::string& p_Text) {
    m_LutState = p_State;
    std::lock_guard<std::mutex> s_Lock(m_LutMutex);
    m_LutStatus = p_Text;
}

void BetterInstinct::ReleaseLut() {
    delete m_Lut;
    m_Lut = nullptr;
    m_LutApplied = false;
    m_LutAppliedSig = 0;
    m_LutFailSig.store(0);
}

// Still the same resource, loaded and unchanged? (checked every frame, it is just a few reads)
bool BetterInstinct::LutStillValid() const {
    if (!m_Lut || !Globals::ResourceContainer || !*Globals::ResourceContainer) return false;
    auto& s_Resources = (*Globals::ResourceContainer)->m_resources;
    if (m_Lut->m_Index >= s_Resources.size()) return false;
    const auto& s_Info = s_Resources[m_Lut->m_Index];
    return s_Info.rid.GetID() == kInstinctLutId && s_Info.status == RESOURCE_STATUS_VALID && s_Info.resourceData == m_Lut->m_Data;
}

// Finds the LUT texture and gets the vanilla pixels. Returns true when m_Lut is ready to use.
bool BetterInstinct::EnsureLut() {
    if (m_Lut && LutStillValid()) return true;
    if (m_Lut) ReleaseLut(); // the game reloaded the texture (new mission): start again on the fresh one

    if (!Globals::ResourceContainer || !*Globals::ResourceContainer) {
        SetLutStatus(2, "The game's resource container is not available.");
        return false;
    }

    auto& s_Resources = (*Globals::ResourceContainer)->m_resources;
    const size_t s_Count = s_Resources.size();
    bool s_FoundButNotLoaded = false;

    for (size_t i = 0; i < s_Count; ++i) {
        const auto& s_Info = s_Resources[i];
        if (s_Info.rid.GetID() != kInstinctLutId) continue;

        if (s_Info.status != RESOURCE_STATUS_VALID || !s_Info.resourceData) {
            s_FoundButNotLoaded = true;
            continue;
        }

        std::string s_How;
        ID3D12Resource* s_Res = LutProbe(s_Info.resourceData, s_How);
        char s_Buf[256];
        if (!s_Res) {
            std::snprintf(s_Buf, sizeof(s_Buf),
                          "Found the Instinct LUT (resource #%zu, type 0x%08X, data %p) but no 64x64 D3D12 texture inside it.", i,
                          s_Info.resourceType, s_Info.resourceData);
            SetLutStatus(2, s_Buf);
            return false;
        }

        auto* s_Gpu = new SLutGpu();
        s_Gpu->m_Index = static_cast<uint32_t>(i);
        s_Gpu->m_Data = s_Info.resourceData;
        s_Gpu->m_How = s_How;

        std::string s_Err;
        if (!LutInit(*s_Gpu, s_Res, s_Err)) {
            delete s_Gpu;
            SetLutStatus(2, "Instinct LUT: " + s_Err + ".");
            return false;
        }

        // Vanilla pixels: from the cache file when there is one (so a plugin reload can never mistake our own
        // recolored texture for the vanilla one), otherwise read back from the GPU and cache.
        const size_t s_Bytes = static_cast<size_t>(s_Gpu->m_Desc.Width) * s_Gpu->m_Desc.Height * 4;
        const fs::path s_Cache = LutCachePath();
        bool s_Have = false;
        if (!s_Cache.empty()) {
            std::ifstream s_In(s_Cache, std::ios::binary);
            if (s_In) {
                std::vector<uint8_t> s_Data((std::istreambuf_iterator<char>(s_In)), std::istreambuf_iterator<char>());
                if (s_Data.size() == s_Bytes) {
                    s_Gpu->m_Base = std::move(s_Data);
                    s_Have = true;
                }
            }
        }
        if (!s_Have) {
            if (!LutReadback(*s_Gpu, m_LutBarrier, s_Err)) {
                delete s_Gpu;
                SetLutStatus(2, "Instinct LUT: " + s_Err + ".");
                return false;
            }
            if (!s_Cache.empty()) {
                std::error_code s_Ec;
                fs::create_directories(s_Cache.parent_path(), s_Ec);
                std::ofstream s_Out(s_Cache, std::ios::binary | std::ios::trunc);
                if (s_Out) s_Out.write(reinterpret_cast<const char*>(s_Gpu->m_Base.data()), static_cast<std::streamsize>(s_Gpu->m_Base.size()));
            }
        }

        m_Lut = s_Gpu;
        m_LutApplied = false;
        m_LutAppliedSig = 0;
        std::snprintf(s_Buf, sizeof(s_Buf), "Instinct LUT found (resource #%zu, %ux%u, DXGI %d, %s, %s).", i,
                      static_cast<unsigned>(s_Gpu->m_Desc.Width), s_Gpu->m_Desc.Height, static_cast<int>(s_Gpu->m_Desc.Format),
                      s_How.c_str(), s_Have ? "vanilla pixels from cache" : "vanilla pixels read from the GPU");
        SetLutStatus(3, s_Buf);
        return true;
    }

    SetLutStatus(1, s_FoundButNotLoaded ? "The Instinct LUT exists but is not loaded yet." : "The Instinct LUT is not part of this scene (yet).");
    return false;
}

// Puts the vanilla LUT back and lets go of the texture.
void BetterInstinct::RestoreTint() {
    if (m_Lut && m_LutApplied && m_HasPlayer.load() && LutStillValid()) {
        std::string s_Err;
        LutWrite(*m_Lut, m_LutBarrier, m_Lut->m_Base, s_Err);
    }
    ReleaseLut();
}

// Runs every frame from the game loop (also outside missions).
void BetterInstinct::UpdateTint(float p_Dt, bool /*p_Active*/) {
    if (!m_HasPlayer.load()) {
        // Out of the mission: the texture is about to go away. Drop our references without touching it.
        if (m_NoPlayerTime > 0.25f && m_Lut) ReleaseLut();
        return;
    }

    if (m_LutReread.exchange(false)) {
        RestoreTint(); // vanilla pixels back first, so the new readback is the real vanilla LUT
        std::error_code s_Ec;
        const fs::path s_Cache = LutCachePath();
        if (!s_Cache.empty()) fs::remove(s_Cache, s_Ec);
        m_LutPoll = 0.0f;
    }

    const bool s_Mod = m_TintEnabled || m_TintRemove; // do we want the texture changed at all?
    if (!s_Mod && !m_Lut) return; // nothing to do and nothing to undo

    m_LutPoll -= p_Dt;
    m_LutCooldown -= p_Dt;

    if (m_Lut && !LutStillValid()) ReleaseLut();

    if (!m_Lut) {
        if (!s_Mod || m_LutPoll > 0.0f) return;
        m_LutPoll = 2.0f; // searching walks the whole resource list: only every couple of seconds
        if (!EnsureLut()) return;
    }

    // What should the texture contain right now?
    uint32_t s_Sig = 2166136261u;
    auto s_Mix = [&](uint32_t p_Value) { s_Sig = (s_Sig ^ p_Value) * 16777619u; };
    s_Mix(m_TintRemove ? 2u : (m_TintEnabled ? 1u : 0u));
    if (m_TintEnabled && !m_TintRemove) {
        for (int i = 0; i < 3; ++i) s_Mix(static_cast<uint32_t>(std::lround(std::clamp(m_InstinctTint[i], 0.0f, 1.0f) * 255.0f)));
        s_Mix(static_cast<uint32_t>(std::lround(std::clamp(m_TintStrength, 0.0f, 1.0f) * 255.0f)));
        s_Mix(m_TintKeepBrightness ? 1u : 0u);
        s_Mix(static_cast<uint32_t>(std::lround(m_TintBrightness * 100.0f)));
    }

    if (m_LutApplied && s_Sig == m_LutAppliedSig) return; // already up to date
    if (!m_LutApplied && !s_Mod) {                // vanilla is already in place: let go of the texture
        ReleaseLut();
        return;
    }
    if (s_Sig == m_LutFailSig.load()) return;             // this exact setting failed already: wait for a change

    if (m_LutCooldown > 0.0f) return; // while dragging the color picker, write at most ~10 times per second
    m_LutCooldown = 0.1f;

    std::string s_Err;
    if (s_Mod) {
        std::vector<uint8_t> s_Image;
        bool s_Built = true;
        if (m_TintRemove) {
            s_Image = LutIdentity(m_Lut->m_Base, static_cast<int>(m_Lut->m_Desc.Width), static_cast<int>(m_Lut->m_Desc.Height), s_Err);
            s_Built = !s_Image.empty();
        } else {
            s_Image = LutRecolor(m_Lut->m_Base, m_InstinctTint, std::clamp(m_TintStrength, 0.0f, 1.0f),
                                 m_TintKeepBrightness, m_TintBrightness);
        }
        if (!s_Built) {
            SetLutStatus(2, "Could not remove the Instinct color: " + s_Err + ". The vanilla look stays.");
            m_LutFailSig.store(s_Sig);
        } else if (LutWrite(*m_Lut, m_LutBarrier, s_Image, s_Err)) {
            m_LutApplied = true;
            m_LutAppliedSig = s_Sig;
            ++m_LutWrites;
            SetLutStatus(4, m_TintRemove ? "Instinct color is removed (no grey overlay, only the outlines remain)."
                                         : "Instinct color is active (the game's grey Instinct LUT has been replaced).");
        } else {
            SetLutStatus(2, "Could not write the Instinct LUT: " + s_Err + ". Try the other 'GPU state' option under Advanced.");
            m_LutFailSig.store(s_Sig);
            m_LutApplied = false;
        }
    } else {
        if (LutWrite(*m_Lut, m_LutBarrier, m_Lut->m_Base, s_Err)) {
            m_LutApplied = false;
            m_LutAppliedSig = 0;
            SetLutStatus(3, "Instinct color is off (vanilla LUT restored).");
        }
    }
}

// ---------------------------------------------------------------------------
// HUD meter
// ---------------------------------------------------------------------------

float BetterInstinct::Rand01() {
    m_Rng = m_Rng * 1664525u + 1013904223u;
    return static_cast<float>(m_Rng >> 8) * (1.0f / 16777216.0f);
}

void BetterInstinct::DrawHud() {
    using clock = std::chrono::steady_clock;
    const auto s_Now = clock::now();
    const float s_Dt = std::min(std::chrono::duration<float>(s_Now - m_HudLast).count(), 0.1f);
    m_HudLast = s_Now;

    // In play = the game loop ran recently AND Agent 47 exists. Menus and loading screens hide the meter.
    const int64_t s_SinceUpdate = s_Now.time_since_epoch().count() - m_LastUpdateTicks.load();
    const bool s_InPlay = m_HasPlayer.load() &&
        std::chrono::duration_cast<std::chrono::milliseconds>(clock::duration(s_SinceUpdate)).count() < 250;
    m_InPlay = s_InPlay;
    m_LastHudDt = s_Dt;
    const bool s_Preview = m_ShowUI; // while the settings window is open, always show the meter so it can be positioned

    const bool s_Active = m_Active.load();

    float s_TargetAlpha = 0.0f;
    if (m_HudEnabled && (s_InPlay || s_Preview)) {
        if (s_Preview || m_HudAlways || s_Active || m_Blocked || m_HoldVisible > 0.0f) {
            s_TargetAlpha = 1.0f;
        }
    }
    m_HudAlpha += (s_TargetAlpha - m_HudAlpha) * std::clamp(8.0f * s_Dt, 0.0f, 1.0f);
    if (m_HudAlpha < 0.01f) {
        m_Particles.clear();
        return;
    }

    const ImVec2 s_Screen = ImGui::GetIO().DisplaySize;
    const float s_Ui = (s_Screen.y / 1080.0f) * m_HudScale;
    const float s_Thick = std::max(1.0f, std::round(s_Ui));
    const bool s_Vert = m_HudVertical;
    const float s_BarLen = s_Screen.x * m_HudWidth * m_HudScale; // length of the bar along its direction
    const float s_Girth = std::max(5.0f, 9.0f * s_Ui);           // thickness of the bar

    // Position: the bar's center across the screen, and its lower end measured from the bottom. Kept on screen.
    const float s_HalfAcross = (s_Vert ? s_Girth : s_BarLen) * 0.5f;
    const float s_Cx = std::clamp(s_Screen.x * std::clamp(m_HudX, 0.0f, 1.0f), s_HalfAcross,
                                  std::max(s_HalfAcross, s_Screen.x - s_HalfAcross));
    const float s_IconR = std::max(7.0f, 12.0f * s_Ui);
    const float s_LabelRoom = 4.0f * s_Ui;
    const float s_BelowRoom = (s_Vert && m_ShowIcon) ? (s_IconR * 2.0f + 6.0f * s_Ui) : 0.0f; // vertical bar: the icon sits under it
    const float s_MinBottom = (s_Vert ? s_BarLen : s_Girth) + s_LabelRoom;
    const float s_Bottom = std::clamp(s_Screen.y - m_HudYOffset * (s_Screen.y / 1080.0f), std::min(s_MinBottom, s_Screen.y - s_BelowRoom), s_Screen.y - s_BelowRoom);
    const float s_Top = s_Bottom - (s_Vert ? s_BarLen : s_Girth);
    const float s_Left = s_Cx - (s_Vert ? s_Girth : s_BarLen) * 0.5f;
    const float s_Right = s_Cx + (s_Vert ? s_Girth : s_BarLen) * 0.5f;

    const float s_Time = std::chrono::duration<float>(s_Now.time_since_epoch()).count();
    const float s_A = m_HudAlpha * m_HudOpacity;
    ImDrawList* s_Dl = ImGui::GetBackgroundDrawList();

    auto s_Col = [](float r, float g, float b, float a) {
        return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, std::clamp(a, 0.0f, 1.0f)));
    };

    // Fill color: gold normally, blends to the "low" color when nearly empty, pulses while recharging.
    const float s_Low = 1.0f - std::clamp(m_Meter / kLowLevel, 0.0f, 1.0f);
    // Bar colors: the bottom color at the start of the bar, the top color at the end. The fill shows the part of that
    // gradient up to the current level. Both blend to the "low" color when nearly empty; recharging uses the low color.
    float s_ColBot[3], s_ColTop[3];
    for (int k = 0; k < 3; ++k) {
        s_ColBot[k] = m_BarBottom[k] + (m_LowColor[k] - m_BarBottom[k]) * s_Low;
        s_ColTop[k] = m_BarTop[k] + (m_LowColor[k] - m_BarTop[k]) * s_Low;
        if (m_Blocked) { s_ColBot[k] = m_LowColor[k]; s_ColTop[k] = m_LowColor[k]; }
    }
    float s_FillA = s_A;
    if (m_Blocked) s_FillA = s_A * (0.55f + 0.35f * std::sin(s_Time * 8.0f));
    const float s_Fill = std::clamp(m_Meter, 0.0f, 1.0f);
    const float s_R = s_ColBot[0] + (s_ColTop[0] - s_ColBot[0]) * s_Fill; // color at the tip of the fill
    const float s_G = s_ColBot[1] + (s_ColTop[1] - s_ColBot[1]) * s_Fill;
    const float s_B = s_ColBot[2] + (s_ColTop[2] - s_ColBot[2]) * s_Fill;

    // Dark backing, then the grey "empty" track.
    const float s_Inset = s_Thick * 2.0f;
    const float s_InnerL = s_Left + s_Inset;
    const float s_InnerR = s_Right - s_Inset;
    const float s_InnerT = s_Top + s_Inset;
    const float s_InnerB = s_Bottom - s_Inset;
    const float s_MidX = (s_InnerL + s_InnerR) * 0.5f;
    const float s_MidY = (s_InnerT + s_InnerB) * 0.5f;

    s_Dl->AddRectFilled(ImVec2(s_Left, s_Top), ImVec2(s_Right, s_Bottom), IM_COL32(10, 10, 12, static_cast<int>(170 * s_A)));
    s_Dl->AddRectFilled(ImVec2(s_InnerL, s_InnerT), ImVec2(s_InnerR, s_InnerB), IM_COL32(88, 88, 92, static_cast<int>(120 * s_A)));

    // Where the fill ends (the "tip"): horizontal bars fill left to right, vertical bars bottom to top.
    const float s_HeadX = s_Vert ? s_MidX : s_InnerL + (s_InnerR - s_InnerL) * m_Meter;
    const float s_HeadY = s_Vert ? s_InnerB - (s_InnerB - s_InnerT) * m_Meter : s_MidY;

    // Fill: darker at the start, brighter toward the tip.
    const bool s_HasFill = s_Vert ? (s_InnerB - s_HeadY > 0.5f) : (s_HeadX - s_InnerL > 0.5f);
    if (s_HasFill) {
        const ImU32 s_Dim = s_Col(s_ColBot[0], s_ColBot[1], s_ColBot[2], s_FillA);
        const ImU32 s_Bright = s_Col(s_R, s_G, s_B, s_FillA);
        const ImU32 s_Shine = IM_COL32(255, 255, 255, static_cast<int>(70 * s_A));
        if (s_Vert) {
            s_Dl->AddRectFilledMultiColor(ImVec2(s_InnerL, s_HeadY), ImVec2(s_InnerR, s_InnerB), s_Bright, s_Bright, s_Dim, s_Dim);
            s_Dl->AddRectFilled(ImVec2(s_InnerL, s_HeadY), ImVec2(s_InnerL + s_Thick, s_InnerB), s_Shine); // highlight along the left edge
        } else {
            s_Dl->AddRectFilledMultiColor(ImVec2(s_InnerL, s_InnerT), ImVec2(s_HeadX, s_InnerB), s_Dim, s_Bright, s_Bright, s_Dim);
            s_Dl->AddRectFilled(ImVec2(s_InnerL, s_InnerT), ImVec2(s_HeadX, s_InnerT + s_Thick), s_Shine); // highlight along the top
        }
    }

    // Sparkles at the tip: only while Instinct is in use. Tiny firework-like sparks burst out of the tip in every
    // direction, slow down fast and die quickly, so the effect stays small and concentrated (no rising flames).
    const bool s_Emit = s_Active && m_Meter > 0.004f;
    const float s_PxMin = std::max(1.0f, s_Ui);
    if (!s_Active) {
        m_Particles.clear(); // not using Instinct: plain bar, no effects
        m_EmitAcc = 0.0f;
    }
    if (s_Emit) {
        m_EmitAcc += s_Dt * (m_FireFx ? 70.0f * m_FireAmount : 60.0f);
        const size_t s_MaxParticles = 220;
        while (m_EmitAcc >= 1.0f) {
            m_EmitAcc -= 1.0f;
            if (m_Particles.size() >= s_MaxParticles) break;

            Particle s_P;
            const float s_Ang = Rand01() * 6.2831853f;
            const float s_Speed = (18.0f + 62.0f * Rand01() * Rand01()) * s_Ui; // most sparks are slow, a few shoot out further
            s_P.x = s_HeadX + std::cos(s_Ang) * 1.0f * s_Ui;
            s_P.y = s_HeadY + std::sin(s_Ang) * 1.0f * s_Ui;
            s_P.vx = std::cos(s_Ang) * s_Speed;
            s_P.vy = std::sin(s_Ang) * s_Speed;
            s_P.phase = Rand01() * 6.2831853f;
            s_P.ember = Rand01() < 0.25f; // "ember" = a brighter spark with a short trail
            s_P.maxLife = 0.22f + 0.38f * Rand01();
            s_P.size = (0.7f + 0.9f * Rand01()) * s_PxMin;
            s_P.life = s_P.maxLife;
            m_Particles.push_back(s_P);
        }
    } else {
        m_EmitAcc = 0.0f;
    }

    for (auto& s_P : m_Particles) {
        s_P.life -= s_Dt;
        const float s_Drag = std::max(0.0f, 1.0f - 5.5f * s_Dt); // strong drag: the burst stays near the tip
        s_P.vx *= s_Drag;
        s_P.vy = s_P.vy * s_Drag + 14.0f * s_Ui * s_Dt;          // tiny sag, like a firework spark
        s_P.x += s_P.vx * s_Dt;
        s_P.y += s_P.vy * s_Dt;
    }
    m_Particles.erase(std::remove_if(m_Particles.begin(), m_Particles.end(), [](const Particle& p) { return p.life <= 0.0f; }),
                      m_Particles.end());

    {
        // White-hot at birth, then the meter color, fading out. Each spark twinkles a little.
        const float s_CoreR = s_R + (1.0f - s_R) * 0.8f, s_CoreG = s_G + (1.0f - s_G) * 0.8f, s_CoreB = s_B + (1.0f - s_B) * 0.8f;
        for (const auto& s_P : m_Particles) {
            const float s_K = std::clamp(s_P.life / s_P.maxLife, 0.0f, 1.0f); // 1 = just born, 0 = gone
            const float s_Mix = std::clamp((1.0f - s_K) * 1.8f, 0.0f, 1.0f);
            const float s_Pr = s_CoreR + (s_R - s_CoreR) * s_Mix;
            const float s_Pg = s_CoreG + (s_G - s_CoreG) * s_Mix;
            const float s_Pb = s_CoreB + (s_B - s_CoreB) * s_Mix;
            const float s_Twinkle = 0.65f + 0.35f * std::sin(s_P.phase + s_P.life * 60.0f);
            const float s_Fa = s_K * s_Twinkle * s_A;
            const ImVec2 s_Pos(s_P.x, s_P.y);
            if (s_P.ember) { // short streak along the direction it is flying
                const float s_Sp = std::sqrt(s_P.vx * s_P.vx + s_P.vy * s_P.vy);
                if (s_Sp > 0.5f) {
                    const float s_Tail = std::min(s_Sp * 0.045f, 4.0f * s_Ui);
                    s_Dl->AddLine(ImVec2(s_P.x - s_P.vx / s_Sp * s_Tail, s_P.y - s_P.vy / s_Sp * s_Tail), s_Pos,
                                  s_Col(s_Pr, s_Pg, s_Pb, 0.75f * s_Fa), std::max(1.0f, s_P.size * 0.8f));
                }
            }
            s_Dl->AddCircleFilled(s_Pos, s_P.size * 1.8f, s_Col(s_Pr, s_Pg, s_Pb, 0.14f * s_Fa), 8); // tiny halo
            s_Dl->AddCircleFilled(s_Pos, s_P.size * (0.5f + 0.5f * s_K), s_Col(s_CoreR, s_CoreG, s_CoreB, 0.95f * s_Fa), 6);
        }
    }

    // Thin outline, a little brighter while Instinct is active.
    const float s_Border = s_Active ? 0.55f + 0.2f * std::sin(s_Time * 6.0f) : 0.35f;
    s_Dl->AddRect(ImVec2(s_Left, s_Top), ImVec2(s_Right, s_Bottom), s_Col(1.0f, 0.9f, 0.65f, s_Border * s_A), 0.0f, 0, s_Thick);

    // Round flame icon at the bottom (start) of the bar, like the Absolution Instinct meter.
    if (m_ShowIcon) {
        const ImVec2 s_Ic = s_Vert ? ImVec2(s_Cx, s_Bottom + 4.0f * s_Ui + s_IconR)
                                   : ImVec2(s_Left - 4.0f * s_Ui - s_IconR, (s_Top + s_Bottom) * 0.5f);
        s_Dl->AddCircleFilled(s_Ic, s_IconR, IM_COL32(10, 10, 12, static_cast<int>(190 * s_A)), 32);
        s_Dl->AddCircle(s_Ic, s_IconR, s_Col(s_ColBot[0], s_ColBot[1], s_ColBot[2], 0.9f * s_A), 32, s_Thick * 1.5f);
        auto s_Flame = [&](float p_Scale, float p_Dy, ImU32 p_Color) {
            const float s_F = s_IconR * 0.5f * p_Scale;
            auto P = [&](float x, float y) { return ImVec2(s_Ic.x + x * s_F, s_Ic.y + p_Dy * s_IconR + y * s_F); };
            s_Dl->PathClear();
            s_Dl->PathLineTo(P(0.05f, -1.15f));
            s_Dl->PathBezierCubicCurveTo(P(0.15f, -0.55f), P(0.85f, -0.35f), P(0.70f, 0.30f));
            s_Dl->PathBezierCubicCurveTo(P(0.60f, 0.85f), P(0.25f, 1.05f), P(0.00f, 1.05f));
            s_Dl->PathBezierCubicCurveTo(P(-0.30f, 1.05f), P(-0.75f, 0.80f), P(-0.70f, 0.20f));
            s_Dl->PathBezierCubicCurveTo(P(-0.65f, -0.20f), P(-0.20f, -0.45f), P(0.05f, -1.15f));
            s_Dl->PathFillConvex(p_Color);
        };
        s_Flame(1.0f, -0.02f, s_Col(s_ColBot[0], s_ColBot[1], s_ColBot[2], s_A));
        s_Flame(0.5f, 0.24f, s_Col(s_ColTop[0], s_ColTop[1], s_ColTop[2], s_A));
    }
}

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------

void BetterInstinct::OnDrawMenu() {
    if (ImGui::Button("BETTER INSTINCT")) {
        m_ShowUI = !m_ShowUI;
    }
}

void BetterInstinct::OnDrawUI(bool p_HasFocus) {
    DrawHud();
    UpdateSound(m_LastHudDt, m_InPlay);

    // A file picked in the "Browse..." dialog (it runs on its own thread): use it as the custom sound and save.
    if (g_BrowseReady.exchange(false)) {
        std::wstring s_Picked;
        {
            std::lock_guard<std::mutex> s_Lock(g_BrowseMutex);
            s_Picked = g_BrowseResult;
        }
        const std::string s_Utf8 = WideToUtf8(s_Picked);
        if (!s_Utf8.empty() && s_Utf8.size() < sizeof(m_CustomPath)) {
            std::snprintf(m_CustomPath, sizeof(m_CustomPath), "%s", s_Utf8.c_str());
            m_SoundSource = 1;
            CloseSound();
            m_SoundStatus.clear();
            SaveSettings();
        } else if (!s_Utf8.empty()) {
            m_SoundStatus = "That file path is too long.";
        }
    }

    if (m_GlowDirty.exchange(false)) SaveSettings(); // colors copied from the game on the first scan

    // Safety net: save everything once more whenever the settings window is closed.
    static bool s_WasOpen = false;
    if (m_ShowUI) {
        s_WasOpen = true;
    } else if (s_WasOpen) {
        s_WasOpen = false;
        SaveSettings();
    }

    // Outside a mission the game loop doesn't run, so never leave the game-level block switched on.
    if (!m_InPlay) {
        m_InputBlock = false;
    }

    // Action names are only recorded while the Debug section is open (set again below if it is).
    m_CaptureNames = false;

    if (!m_ShowUI) {
        m_Binding = false;
        m_ForceBlockTest = false;
        if (m_Testing) {
            m_Testing = false;
        }
        return;
    }

    bool s_Changed = false;

    ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Better Instinct", &m_ShowUI)) {
        // --- Key binding ---
        ImGui::TextUnformatted("Instinct key (set this to your in-game Instinct bind)");
        if (m_Binding) {
            ImGui::Button("Press a key or mouse button... (Esc cancels)", ImVec2(-1, 0));

            bool s_AnyDown = false;
            for (int vk = 1; vk < 255; ++vk) {
                if (vk >= VK_SHIFT && vk <= VK_MENU) continue; // generic modifiers; L/R variants are captured instead
                if (IsKeyDown(vk)) { s_AnyDown = true; break; }
            }

            if (!m_BindArmed) {
                m_BindArmed = !s_AnyDown; // wait for the click that started binding to be released
            } else {
                for (int vk = 1; vk < 255; ++vk) {
                    if (vk >= VK_SHIFT && vk <= VK_MENU) continue;
                    if (!IsKeyDown(vk)) continue;

                    if (vk != VK_ESCAPE) {
                        m_InstinctKey = vk;
                        s_Changed = true;
                    }
                    m_Binding = false;
                    break;
                }
            }
        } else {
            const std::string s_Text = KeyName(m_InstinctKey) + "  (click to change)";
            if (ImGui::Button(s_Text.c_str(), ImVec2(-1, 0))) {
                m_Binding = true;
                m_BindArmed = false;
            }
        }

        int s_Mode = m_ToggleMode ? 1 : 0;
        if (ImGui::RadioButton("Hold", &s_Mode, 0)) { m_ToggleMode = false; s_Changed = true; }
        ImGui::SameLine();
        if (ImGui::RadioButton("Toggle", &s_Mode, 1)) { m_ToggleMode = true; s_Changed = true; }
        ImGui::SameLine();
        ImGui::TextDisabled("(match your in-game Instinct setting)");

        if (ImGui::Checkbox("Block Instinct when the meter is too low", &m_BlockEnabled)) s_Changed = true;
        ImGui::TextDisabled(g_HookOk.load() ? "Input guard: active" : "Input guard: NOT active (blocking unavailable)");

        if (ImGui::InputText("Instinct action name(s)", m_ActionPatterns, sizeof(m_ActionPatterns))) {
            RebuildPatterns();
            s_Changed = true;
        }
        ImGui::TextDisabled("Comma-separated, matches part of the game's action name.");

        ImGui::Separator();

        // --- Slowdown & meter ---
        if (ImGui::SliderFloat("Time scale", &m_FocusScale, 0.05f, 0.95f, "%.2fx")) s_Changed = true;
        if (ImGui::SliderFloat("Transition speed", &m_BlendSpeed, 1.0f, 30.0f, "%.1f")) s_Changed = true;
        if (ImGui::SliderFloat("Drain time (full to empty)", &m_DrainSeconds, 2.0f, 60.0f, "%.0f s")) s_Changed = true;
        if (ImGui::SliderFloat("Recharge time (empty to full)", &m_RechargeSeconds, 2.0f, 120.0f, "%.0f s")) s_Changed = true;
        if (ImGui::SliderFloat("Recharge delay", &m_RechargeDelay, 0.0f, 5.0f, "%.1f s")) s_Changed = true;

        if (ImGui::Button("Refill meter")) {
            m_Meter = 1.0f;
            m_Locked = false;
        }

        ImGui::Separator();

        // --- Instinct color ---
        if (ImGui::Checkbox("Remove the Instinct color completely (no overlay, outlines only)", &m_TintRemove)) s_Changed = true;
        ImGui::TextDisabled("The screen stays exactly as it is without Instinct; the game's outlines on people and items still show.");
        ImGui::BeginDisabled(m_TintRemove);
        if (ImGui::Checkbox("Custom Instinct color (replaces the grey)", &m_TintEnabled)) s_Changed = true;
        ImGui::EndDisabled();
        ImGui::BeginDisabled(!m_TintEnabled || m_TintRemove);
        if (ImGui::ColorEdit3("Instinct color", m_InstinctTint)) s_Changed = true;
        if (ImGui::SliderFloat("Strength", &m_TintStrength, 0.0f, 1.0f, "%.2f")) s_Changed = true;
        ImGui::TextDisabled("1.00 = the grey is completely replaced by your color. Lower keeps part of the original look.");
        if (ImGui::SliderFloat("Brightness", &m_TintBrightness, 0.25f, 3.0f, "%.2f")) s_Changed = true;
        if (ImGui::Checkbox("Keep brightness (recommended)", &m_TintKeepBrightness)) s_Changed = true;
        if (ImGui::Button("Reset color")) {
            m_InstinctTint[0] = 0.30f;
            m_InstinctTint[1] = 0.70f;
            m_InstinctTint[2] = 1.00f;
            m_TintStrength = 1.0f;
            m_TintBrightness = 1.0f;
            m_TintKeepBrightness = true;
            s_Changed = true;
        }
        ImGui::EndDisabled();

        {
            std::string s_LutText;
            {
                std::lock_guard<std::mutex> s_LutLock(m_LutMutex);
                s_LutText = m_LutStatus;
            }
            const int s_LutState = m_LutState.load();
            const ImVec4 s_LutColor = s_LutState == 4 ? ImVec4(0.45f, 0.85f, 0.45f, 1.0f)
                                    : s_LutState == 2 ? ImVec4(0.95f, 0.45f, 0.40f, 1.0f)
                                                      : ImVec4(0.70f, 0.70f, 0.70f, 1.0f);
            if (s_LutText.empty()) s_LutText = (m_TintEnabled || m_TintRemove) ? "Waiting for a mission to load the Instinct texture..." : "Off.";
            ImGui::PushStyleColor(ImGuiCol_Text, s_LutColor);
            ImGui::TextWrapped("%s", s_LutText.c_str());
            ImGui::PopStyleColor();
        }

        if (ImGui::TreeNode("Advanced (Instinct color)")) {
            ImGui::TextDisabled("Instinct's grey is the game's color-grading texture 008C1F5C7305A978. It is recolored on the GPU,");
            ImGui::TextDisabled("so only the 3D scene changes. It is only used while Instinct is on.");
            const char* s_States[] = {"Shader resource (default)", "Common"};
            if (ImGui::Combo("GPU texture state", &m_LutBarrier, s_States, 2)) {
                m_LutFailSig.store(0);
                s_Changed = true;
            }
            ImGui::TextDisabled("Only change this if the color does not apply or the picture glitches.");
            if (ImGui::Button("Re-read vanilla texture")) m_LutReread = true;
            ImGui::SameLine();
            ImGui::TextDisabled("(after a game update)");
            ImGui::TreePop();
        }

        ImGui::Separator();

        // --- Better Zoom ---
        if (ImGui::CollapsingHeader("Zoom (right mouse + mouse wheel)")) {
            if (ImGui::Checkbox("Enable wheel zoom", &m_ZoomEnabled)) s_Changed = true;
            ImGui::TextDisabled("Hold right mouse with empty hands (or an item you can't throw), then scroll.");
            if (ImGui::SliderFloat("Max zoom", &m_ZoomMax, 1.5f, 10.0f, "%.1fx")) s_Changed = true;
            if (ImGui::SliderFloat("Step per wheel notch", &m_ZoomStep, 1.03f, 1.6f, "x%.2f")) s_Changed = true;
            if (ImGui::SliderFloat("Smoothing", &m_ZoomSmooth, 2.0f, 40.0f, "%.0f")) s_Changed = true;
            if (ImGui::Checkbox("Remember zoom level between aims", &m_ZoomRemember)) s_Changed = true;
            if (ImGui::Checkbox("Zoom with any item (temporary, no item check yet)", &m_ZoomAnyItem)) s_Changed = true;
            if (ImGui::Button("Dump inventory info to log")) m_DumpRequest = true;
            ImGui::TextDisabled("Hold the item first, press the button, then send me BetterInstinct.log (next to the config).");
            ImGui::TextDisabled("Camera profiles found: %u   Current zoom: %.2fx", m_ZoomFound.load(), m_ZoomCur);
        }

        ImGui::Separator();

        // --- Instinct glow colors (people and items) ---
        if (ImGui::CollapsingHeader("Instinct colors (people and items)")) {
            if (ImGui::Button("All to game default")) {
                for (auto& s_G : m_Glow) s_G.Mode = 0;
                s_Changed = true;
            }
            ImGui::TextDisabled("Pick \"Custom\" for a group, then choose the silhouette (fill) and outline colors. Applies in missions.");
            ImGui::TextDisabled("The colors are written while a mission loads. If you change them mid-mission and nothing changes, reload the mission.");
            {
                const int s_GlowSt = m_GlowState.load();
                ImGui::TextDisabled("Status: %s | written at load: %u | colors found now: %u",
                                    s_GlowSt == 2 ? "colors found" : (s_GlowSt == 1 ? "config not found yet" : "waiting for a mission"),
                                    m_GlowPreApplied.load(), m_GlowFound.load());
                ImGui::TextDisabled("Template hook: ran %u times | config entity seen %u times",
                                    m_GlowTplCalls.load(), m_GlowTplHits.load());
            }
            ImGui::TextDisabled("Unticking Silhouette / Outline hides that part. Alpha = how strong it is.");

            for (int i = 0; i < kGlowCatCount; ++i) {
                SGlowSetting& s_G = m_Glow[i];
                ImGui::PushID(i);

                std::string s_Title = kGlowCats[i].Label;
                if (s_G.Mode != 0) s_Title += "  (custom)";
                s_Title += "###glowcat";

                if (ImGui::TreeNode(s_Title.c_str())) {
                    ImGui::TextDisabled("%s", kGlowCats[i].Tip);

                    static const char* const s_ModeNames[] = {"Game default", "Custom"};
                    if (ImGui::Combo("Colors", &s_G.Mode, s_ModeNames, 2)) s_Changed = true;

                    ImGui::BeginDisabled(s_G.Mode == 0);
                    constexpr ImGuiColorEditFlags s_Flags = ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf;

                    if (ImGui::Checkbox("Silhouette", &s_G.FillOn)) { s_G.Seeded = true; s_Changed = true; }
                    ImGui::SameLine();
                    if (ImGui::ColorEdit4("##fill", s_G.Fill, s_Flags)) { s_G.Seeded = true; s_Changed = true; }

                    if (ImGui::Checkbox("Outline   ", &s_G.OutlineOn)) { s_G.Seeded = true; s_Changed = true; }
                    ImGui::SameLine();
                    if (ImGui::ColorEdit4("##outline", s_G.Outline, s_Flags)) { s_G.Seeded = true; s_Changed = true; }

                    ImGui::BeginDisabled(!s_G.HasVanilla);
                    if (ImGui::Button("Copy the game's colors")) {
                        std::memcpy(s_G.Fill, s_G.VanFill, sizeof(s_G.Fill));
                        std::memcpy(s_G.Outline, s_G.VanOutline, sizeof(s_G.Outline));
                        for (int k = 0; k < 4; ++k) {
                            s_G.Fill[k] = std::clamp(s_G.Fill[k], 0.0f, 1.0f);
                            s_G.Outline[k] = std::clamp(s_G.Outline[k], 0.0f, 1.0f);
                        }
                        s_G.FillOn = true;
                        s_G.OutlineOn = true;
                        s_G.Seeded = true;
                        s_Changed = true;
                    }
                    ImGui::EndDisabled();
                    ImGui::EndDisabled();
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
        }
        ImGui::Separator();

        // --- HUD ---
        if (ImGui::Checkbox("Show Instinct meter", &m_HudEnabled)) s_Changed = true;
        ImGui::BeginDisabled(!m_HudEnabled);
        if (ImGui::Checkbox("Always visible (even when full)", &m_HudAlways)) s_Changed = true;
        {
            static const char* const s_OrientNames[] = {"Horizontal", "Vertical"};
            int s_OrientIdx = m_HudVertical ? 1 : 0;
            if (ImGui::Combo("Orientation", &s_OrientIdx, s_OrientNames, 2)) {
                m_HudVertical = (s_OrientIdx == 1);
                s_Changed = true;
            }
        }
        if (ImGui::SliderFloat("Length", &m_HudWidth, 0.05f, 0.5f, "%.2f")) s_Changed = true;
        if (ImGui::SliderFloat("Horizontal position", &m_HudX, 0.0f, 1.0f, "%.2f")) s_Changed = true;
        if (ImGui::SliderFloat("Height from bottom", &m_HudYOffset, 0.0f, 1080.0f, "%.0f px")) s_Changed = true;
        ImGui::TextDisabled("Horizontal position: 0 = left edge, 1 = right edge. A vertical bar fills upward.");
        if (ImGui::Button("Bottom center")) { m_HudX = 0.5f; m_HudYOffset = 64.0f; s_Changed = true; }
        ImGui::SameLine();
        if (ImGui::Button("Top center")) { m_HudX = 0.5f; m_HudYOffset = 1010.0f; s_Changed = true; }
        ImGui::SameLine();
        if (ImGui::Button("Left side")) { m_HudX = 0.04f; m_HudYOffset = 380.0f; s_Changed = true; }
        ImGui::SameLine();
        if (ImGui::Button("Right side")) { m_HudX = 0.96f; m_HudYOffset = 380.0f; s_Changed = true; }
        if (ImGui::SliderFloat("Size", &m_HudScale, 0.5f, 3.0f, "%.2f")) s_Changed = true;
        if (ImGui::SliderFloat("Opacity", &m_HudOpacity, 0.1f, 1.0f, "%.2f")) s_Changed = true;
        if (ImGui::Checkbox("Fire effect at the tip (like Absolution)", &m_FireFx)) s_Changed = true;
        ImGui::BeginDisabled(!m_FireFx);
        if (ImGui::SliderFloat("Fire amount", &m_FireAmount, 0.2f, 2.5f, "%.2f")) s_Changed = true;
        ImGui::EndDisabled();
        if (ImGui::ColorEdit3("Bar color: bottom", m_BarBottom)) s_Changed = true;
        if (ImGui::ColorEdit3("Bar color: top", m_BarTop)) s_Changed = true;
        if (ImGui::Checkbox("Flame icon at the bottom of the bar", &m_ShowIcon)) s_Changed = true;
        if (ImGui::ColorEdit3("Low-meter color", m_LowColor)) s_Changed = true;
        ImGui::EndDisabled();

        ImGui::Separator();

        // --- Sound ---
        if (ImGui::Checkbox("Instinct sound", &m_SoundEnabled)) {
            s_Changed = true;
            if (!m_SoundEnabled) m_Testing = false;
        }

        ImGui::BeginDisabled(!m_SoundEnabled);
        static const char* const s_SourceNames[] = {"Preset / folder", "Custom file"};
        if (ImGui::Combo("Source", &m_SoundSource, s_SourceNames, 2)) {
            s_Changed = true;
            CloseSound();
        }

        ImGui::BeginDisabled(g_BrowseBusy.load());
        if (ImGui::Button(g_BrowseBusy.load() ? "Choose a file in the dialog..." : "Browse for your own sound...")) {
            std::wstring s_StartDir = m_SoundDir;
            if (m_CustomPath[0]) {
                std::error_code s_BrowseEc;
                const fs::path s_Parent = fs::path(Utf8ToWide(m_CustomPath)).parent_path();
                if (!s_Parent.empty() && fs::is_directory(s_Parent, s_BrowseEc)) s_StartDir = s_Parent.wstring();
            }
            StartBrowse(s_StartDir);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled(".wav / .mp3");

        if (m_SoundSource == 0) {
            const char* s_Current = (m_PresetIndex >= 0 && m_PresetIndex < static_cast<int>(m_PresetFiles.size()))
                                        ? m_PresetFiles[m_PresetIndex].c_str() : "(no sounds found)";
            if (ImGui::BeginCombo("Sound", s_Current)) {
                for (int i = 0; i < static_cast<int>(m_PresetFiles.size()); ++i) {
                    if (ImGui::Selectable(m_PresetFiles[i].c_str(), i == m_PresetIndex)) {
                        m_PresetIndex = i;
                        m_PresetName = m_PresetFiles[i];
                        CloseSound();
                        s_Changed = true;
                    }
                }
                ImGui::EndCombo();
            }
            if (ImGui::Button("Rescan folder")) {
                ScanSounds();
                CloseSound();
            }
            ImGui::SameLine();
            ImGui::TextDisabled("mods\\InstinctSounds\\ (.wav / .mp3)");
        } else {
            if (ImGui::InputText("File path", m_CustomPath, sizeof(m_CustomPath))) {
                CloseSound();
                s_Changed = true;
            }
            ImGui::TextDisabled("Chosen with Browse, or paste the full path to a .wav or .mp3 file");
        }

        if (ImGui::SliderFloat("Volume", &m_SoundVolume, 0.0f, 1.0f, "%.2f")) {
            ApplySoundVolume();
            s_Changed = true;
        }

        ImGui::TextUnformatted("When instinct is released:");
        int s_Release = m_FadeOutOn ? 1 : 0;
        if (ImGui::RadioButton("Stop instantly", &s_Release, 0)) { m_FadeOutOn = false; s_Changed = true; }
        ImGui::SameLine();
        if (ImGui::RadioButton("Fade out", &s_Release, 1)) { m_FadeOutOn = true; s_Changed = true; }
        if (m_FadeOutOn) {
            if (ImGui::SliderFloat("Fade-out (ms)", &m_FadeOutMs, 50.0f, 3000.0f, "%.0f")) s_Changed = true;
        }

        if (ImGui::Checkbox("Fade in on activate", &m_FadeInOn)) s_Changed = true;
        if (m_FadeInOn) {
            if (ImGui::SliderFloat("Fade-in (ms)", &m_FadeInMs, 50.0f, 3000.0f, "%.0f")) s_Changed = true;
        }

        if (!m_Testing) {
            if (ImGui::Button("Test sound")) m_Testing = true;
        } else {
            if (ImGui::Button("Stop test")) m_Testing = false;
        }
        if (!m_SoundStatus.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "%s", m_SoundStatus.c_str());
        }
        ImGui::EndDisabled();
    }
    ImGui::End();

    if (s_Changed) {
        SaveSettings();
    }
}

DEFINE_ZHM_PLUGIN(BetterInstinct)
