#include "BetterInstinct.h"

#include <Windows.h>
#include <mmsystem.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include <imgui.h>

#include "Functions.h"
#include "Hooks.h"

#include "Glacier/ZActor.h"
#include "Glacier/ZCameraEntity.h"
#include "Glacier/ZCollision.h"
#include "Glacier/ZEntity.h"
#include "Glacier/ZGameLoopManager.h"
#include "Glacier/ZHitman5.h"
#include "Glacier/ZObject.h"
#include "Glacier/ZGameTime.h"
#include "Glacier/ZInput.h"

static constexpr char kSection[] = "BetterInstinct";
static constexpr float kUnlockLevel = 0.25f; // after running dry, the meter must refill to this before Instinct works again
static constexpr float kMinStart = 0.10f;    // Instinct can't be started below this level
static constexpr float kLowLevel = 0.25f;    // below this the meter turns to the "low" color

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
std::atomic<int> g_Key{0x10};
std::atomic<bool> g_Phys[256];
std::atomic<bool> g_HookOk{false};
std::atomic<bool> g_HookReady{false};
std::atomic<DWORD> g_HookThreadId{0};
DWORD g_Pid = 0;
std::atomic<uint32_t> g_KbEvents{0};
std::atomic<uint32_t> g_MouseEvents{0};
std::atomic<int> g_LastVk{0};
constexpr ULONG_PTR kOurMarker = 0x464F4341; // tags the key events we inject ourselves, so the hook can ignore them

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

    CloseSound();
    g_Guard.reset(); // removes the input hooks

    if (m_Touched && Globals::GameTimeManager) {
        Globals::GameTimeManager->m_fGameTimeMultiplier = 1.0f;
    }
}

void BetterInstinct::Init() {
    // Lets us answer "not pressed" to the game when it asks about the Instinct action.
    Hooks::ZInputAction_Digital->AddDetour(this, &BetterInstinct::ZInputAction_Digital);
}

void BetterInstinct::OnEngineInitialized() {
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
    if (!s_ConfigFound) SaveConfigFile();
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
    m_HudYOffset = std::clamp(static_cast<float>(GetSettingDouble(kSection, "hud_y", m_HudYOffset)), 0.0f, 600.0f);
    m_HudScale = std::clamp(static_cast<float>(GetSettingDouble(kSection, "hud_scale", m_HudScale)), 0.5f, 3.0f);
    m_HudOpacity = std::clamp(static_cast<float>(GetSettingDouble(kSection, "hud_opacity", m_HudOpacity)), 0.1f, 1.0f);
    m_FillColor[0] = std::clamp(static_cast<float>(GetSettingDouble(kSection, "fill_r", m_FillColor[0])), 0.0f, 1.0f);
    m_FillColor[1] = std::clamp(static_cast<float>(GetSettingDouble(kSection, "fill_g", m_FillColor[1])), 0.0f, 1.0f);
    m_FillColor[2] = std::clamp(static_cast<float>(GetSettingDouble(kSection, "fill_b", m_FillColor[2])), 0.0f, 1.0f);
    m_LowColor[0] = std::clamp(static_cast<float>(GetSettingDouble(kSection, "low_r", m_LowColor[0])), 0.0f, 1.0f);
    m_LowColor[1] = std::clamp(static_cast<float>(GetSettingDouble(kSection, "low_g", m_LowColor[1])), 0.0f, 1.0f);
    m_LowColor[2] = std::clamp(static_cast<float>(GetSettingDouble(kSection, "low_b", m_LowColor[2])), 0.0f, 1.0f);

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
    SetSettingDouble(kSection, "hud_scale", m_HudScale);
    SetSettingDouble(kSection, "hud_opacity", m_HudOpacity);
    SetSettingDouble(kSection, "fill_r", m_FillColor[0]);
    SetSettingDouble(kSection, "fill_g", m_FillColor[1]);
    SetSettingDouble(kSection, "fill_b", m_FillColor[2]);
    SetSettingDouble(kSection, "low_r", m_LowColor[0]);
    SetSettingDouble(kSection, "low_g", m_LowColor[1]);
    SetSettingDouble(kSection, "low_b", m_LowColor[2]);

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

        s_Bool("ps_enabled", m_PsEnabled);
        s_Int("ps_key", m_PsKey);
        s_Flt("ps_scale", m_PsScale);
        s_Flt("ps_tag_cost", m_PsTagCost);
        s_Int("ps_max_tags", m_PsMaxTags);
        s_Flt("ps_shot_delay", m_PsShotDelay);
        s_Flt("ps_drain_mul", m_PsDrainMul);

        s_Bool("hud_enabled", m_HudEnabled);
        s_Bool("hud_always", m_HudAlways);
        s_Flt("hud_width", m_HudWidth);
        s_Flt("hud_y", m_HudYOffset);
        s_Flt("hud_scale", m_HudScale);
        s_Flt("hud_opacity", m_HudOpacity);
        s_Flt("fill_r", m_FillColor[0]);
        s_Flt("fill_g", m_FillColor[1]);
        s_Flt("fill_b", m_FillColor[2]);
        s_Flt("low_r", m_LowColor[0]);
        s_Flt("low_g", m_LowColor[1]);
        s_Flt("low_b", m_LowColor[2]);

        s_Bool("sound_enabled", m_SoundEnabled);
        s_Int("sound_source", m_SoundSource);
        s_Flt("sound_volume", m_SoundVolume);
        s_Bool("fade_out_on", m_FadeOutOn);
        s_Flt("fade_out_ms", m_FadeOutMs);
        s_Bool("fade_in_on", m_FadeInOn);
        s_Flt("fade_in_ms", m_FadeInMs);

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

    s_Bool("ps_enabled", m_PsEnabled);
    s_Int("ps_key", m_PsKey, 1, 254);
    s_Flt("ps_scale", m_PsScale, 0.05f, 0.95f);
    s_Flt("ps_tag_cost", m_PsTagCost, 0.0f, 0.5f);
    s_Int("ps_max_tags", m_PsMaxTags, 1, 12);
    s_Flt("ps_shot_delay", m_PsShotDelay, 0.05f, 1.0f);
    s_Flt("ps_drain_mul", m_PsDrainMul, 0.0f, 2.0f);

    s_Bool("hud_enabled", m_HudEnabled);
    s_Bool("hud_always", m_HudAlways);
    s_Flt("hud_width", m_HudWidth, 0.05f, 0.5f);
    s_Flt("hud_y", m_HudYOffset, 0.0f, 600.0f);
    s_Flt("hud_scale", m_HudScale, 0.5f, 3.0f);
    s_Flt("hud_opacity", m_HudOpacity, 0.1f, 1.0f);
    s_Flt("fill_r", m_FillColor[0], 0.0f, 1.0f);
    s_Flt("fill_g", m_FillColor[1], 0.0f, 1.0f);
    s_Flt("fill_b", m_FillColor[2], 0.0f, 1.0f);
    s_Flt("low_r", m_LowColor[0], 0.0f, 1.0f);
    s_Flt("low_g", m_LowColor[1], 0.0f, 1.0f);
    s_Flt("low_b", m_LowColor[2], 0.0f, 1.0f);

    s_Bool("sound_enabled", m_SoundEnabled);
    s_Int("sound_source", m_SoundSource, 0, 1);
    s_Flt("sound_volume", m_SoundVolume, 0.0f, 1.0f);
    s_Bool("fade_out_on", m_FadeOutOn);
    s_Flt("fade_out_ms", m_FadeOutMs, 0.0f, 3000.0f);
    s_Bool("fade_in_on", m_FadeInOn);
    s_Flt("fade_in_ms", m_FadeInMs, 0.0f, 3000.0f);

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
    const bool s_Wanted = m_SoundEnabled && (((m_Active.load() || m_PsState.load() != PS_IDLE) && p_InPlay) || m_Testing);

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
// Point shooting
// ---------------------------------------------------------------------------

// Raycasts from the middle of the screen (the game camera) and returns the actor that is being aimed at, if any.
ZActor* BetterInstinct::FindAimedActor() {
    if (!Globals::CollisionManager || !*Globals::CollisionManager) {
        return nullptr;
    }

    const auto s_Camera = Functions::GetCurrentCamera->Call();
    if (!s_Camera) {
        return nullptr;
    }

    const SMatrix s_CamMatrix = s_Camera->GetObjectToWorldMatrix();
    const float4 s_Dir = float4(-s_CamMatrix.ZAxis.x, -s_CamMatrix.ZAxis.y, -s_CamMatrix.ZAxis.z, -s_CamMatrix.ZAxis.w);

    // Agent 47's own position: in third person the camera ray first hits him, so we look through him.
    bool s_HavePlayer = false;
    float s_PlayerX = 0.0f, s_PlayerY = 0.0f, s_PlayerZ = 0.0f;
    if (auto s_LocalHitman = SDK()->GetLocalPlayer()) {
        if (auto* s_Spatial = s_LocalHitman.m_entityRef.QueryInterface<ZSpatialEntity>()) {
            const SMatrix s_PlayerMatrix = s_Spatial->GetObjectToWorldMatrix();
            s_PlayerX = s_PlayerMatrix.Trans.x;
            s_PlayerY = s_PlayerMatrix.Trans.y;
            s_PlayerZ = s_PlayerMatrix.Trans.z;
            s_HavePlayer = true;
        }
    }

    float4 s_From = s_CamMatrix.Trans;

    for (int s_Try = 0; s_Try < 3; ++s_Try) {
        ZRayQueryInput s_Input {
            .m_vFrom = s_From,
            .m_vTo = s_From + s_Dir * 500.f,
        };
        ZRayQueryOutput s_Output {};

        if (!(*Globals::CollisionManager)->RayCastClosestHit(s_Input, &s_Output)) {
            return nullptr;
        }
        if (!s_Output.m_pBlockingSpatialEntity.m_pInterfaceRef) {
            return nullptr;
        }

        ZEntityRef s_Parent = s_Output.m_pBlockingSpatialEntity.m_entityRef.GetLogicalParent();
        if (ZActor* s_Actor = s_Parent.QueryInterface<ZActor>()) {
            return s_Actor;
        }

        // Something that isn't an actor was hit. Only look through it when it's right next to Agent 47 (his own body).
        if (!s_HavePlayer) {
            return nullptr;
        }
        const float s_Dx = s_Output.m_vPosition.x - s_PlayerX;
        const float s_Dy = s_Output.m_vPosition.y - s_PlayerY;
        const float s_Dz = s_Output.m_vPosition.z - s_PlayerZ;
        if (s_Dx * s_Dx + s_Dy * s_Dy + s_Dz * s_Dz > 4.0f) {
            return nullptr;
        }
        s_From = s_Output.m_vPosition + s_Dir * 0.3f;
    }

    return nullptr;
}

void BetterInstinct::TryTagTarget() {
    if (static_cast<int>(m_PsTargets.size()) >= m_PsMaxTags) return;
    if (m_Meter < m_PsTagCost) return;

    ZActor* s_Actor = FindAimedActor();
    if (!s_Actor) return;

    for (const ZActor* s_Tagged : m_PsTargets) {
        if (s_Tagged == s_Actor) return; // already tagged
    }

    m_PsTargets.push_back(s_Actor);
    m_PsTagCount = static_cast<int>(m_PsTargets.size());
    m_Meter = std::max(0.0f, m_Meter - m_PsTagCost);
}

// Shoots the next tagged target.
void BetterInstinct::FirePointShot() {
    if (m_PsTargets.empty()) return;

    ZActor* s_Actor = m_PsTargets.front();
    m_PsTargets.erase(m_PsTargets.begin());
    m_PsTagCount = static_cast<int>(m_PsTargets.size());
    if (!s_Actor) return;

    TEntityRef<IItem> s_Item;
    TEntityRef<ZSetpieceEntity> s_SetPieceEntity;

    Functions::ZActor_KillActor->Call(
        s_Actor, s_Item, s_SetPieceEntity, EDamageEvent::eDE_UNDEFINED, EDeathBehavior::eDB_IMPACT_ANIM
    );
}

// A "click to change" key button (same behaviour as the Instinct key button). Returns true when the key changed.
bool BetterInstinct::DrawKeyBinder(const char* p_Id, int& p_Key, bool& p_Binding, bool& p_Armed) {
    bool s_Changed = false;
    ImGui::PushID(p_Id);

    if (p_Binding) {
        ImGui::Button("Press a key or mouse button... (Esc cancels)", ImVec2(-1, 0));

        bool s_AnyDown = false;
        for (int vk = 1; vk < 255; ++vk) {
            if (vk >= VK_SHIFT && vk <= VK_MENU) continue;
            if (IsKeyDown(vk)) { s_AnyDown = true; break; }
        }

        if (!p_Armed) {
            p_Armed = !s_AnyDown; // wait for the click that started binding to be released
        } else {
            for (int vk = 1; vk < 255; ++vk) {
                if (vk >= VK_SHIFT && vk <= VK_MENU) continue;
                if (!IsKeyDown(vk)) continue;

                if (vk != VK_ESCAPE) {
                    p_Key = vk;
                    s_Changed = true;
                }
                p_Binding = false;
                break;
            }
        }
    } else {
        const std::string s_Text = KeyName(p_Key) + "  (click to change)";
        if (ImGui::Button(s_Text.c_str(), ImVec2(-1, 0))) {
            p_Binding = true;
            p_Armed = false;
        }
    }

    ImGui::PopID();
    return s_Changed;
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
    const float s_Dt = std::min(std::chrono::duration<float>(s_Now - m_LastFrame).count(), 0.1f);
    m_LastFrame = s_Now;
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
        m_PsState = PS_IDLE;
        m_PsTargets.clear();
        m_PsTagCount = 0;
        m_PsPrevDown = false;

        if (m_NoPlayerTime > 1.0f) { // really out of the mission (not just a short gap): start the next one fresh
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

    // Only react to keys when the game window is the active window.
    DWORD s_ForegroundPid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &s_ForegroundPid);
    const bool s_GameFocused = s_ForegroundPid == GetCurrentProcessId() && !m_Binding && !m_BindingPs;
    const bool s_KeyDown = s_GameFocused && InstinctKeyDown();
    const bool s_PressEdge = s_KeyDown && !m_PrevDown;

    g_Key = m_InstinctKey;

    // Is Instinct usable right now? Not if the meter ran dry (until it recovers) or is below the start minimum.
    const bool s_WasActive = m_Active.load() || m_PsState.load() != PS_IDLE;
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

    // --- Point shooting ---
    // Hold the key: time slows and whatever you aim at gets tagged (each tag costs some meter).
    // Release the key (or run out of meter): all tagged targets are shot one after another.
    const bool s_PsKeyDown = m_PsEnabled && s_GameFocused && m_PsKey != 0 && PhysKeyDown(m_PsKey);
    switch (m_PsState.load()) {
        case PS_IDLE:
            if (s_PsKeyDown && !m_PsPrevDown && !m_Blocked) {
                m_PsTargets.clear();
                m_PsTagCount = 0;
                m_PsState = PS_AIMING;
            }
            break;

        case PS_AIMING:
            if (!s_PsKeyDown || m_Meter <= 0.0f) {
                if (m_Meter <= 0.0f) {
                    m_Locked = true; // ran dry
                }
                if (m_PsTargets.empty()) {
                    m_PsState = PS_IDLE;
                } else {
                    m_PsShotTimer = 0.05f;
                    m_PsState = PS_EXECUTING;
                }
            } else {
                TryTagTarget();
            }
            break;

        case PS_EXECUTING:
            m_PsShotTimer -= s_Dt;
            if (m_PsShotTimer <= 0.0f) {
                FirePointShot();
                m_PsShotTimer = m_PsShotDelay;
                if (m_PsTargets.empty()) {
                    m_PsState = PS_IDLE;
                }
            }
            break;

        default:
            break;
    }
    m_PsPrevDown = s_PsKeyDown;

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

    // Point shooting takes over completely while it is running.
    if (m_PsState.load() != PS_IDLE) {
        s_Want = false;
        m_ToggleState = false;
    }

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

    // Meter: drains while active, recharges (after a short delay) otherwise.
    const int s_PsNow = m_PsState.load();
    if (s_Want) {
        m_Meter -= s_Dt / m_DrainSeconds;
        m_RechargeWait = m_RechargeDelay;
    } else if (s_PsNow == PS_AIMING) {
        m_Meter -= s_Dt / m_DrainSeconds * m_PsDrainMul;
        m_RechargeWait = m_RechargeDelay;
    } else if (s_PsNow == PS_EXECUTING) {
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
    const bool s_PsSlow = m_PsState.load() != PS_IDLE;
    const float s_Target = s_PsSlow ? m_PsScale : (s_Want ? m_FocusScale : 1.0f);
    const float s_T = std::clamp(m_BlendSpeed * s_Dt, 0.0f, 1.0f);
    m_CurrentScale += (s_Target - m_CurrentScale) * s_T;

    if (s_Want || s_PsSlow || std::abs(m_CurrentScale - 1.0f) > 0.002f) {
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

    const bool s_Active = m_Active.load() || m_PsState.load() != PS_IDLE;

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
    const float s_W = s_Screen.x * m_HudWidth * m_HudScale;
    const float s_H = std::max(5.0f, 9.0f * s_Ui);
    const float s_Cx = s_Screen.x * 0.5f;
    const float s_Bottom = s_Screen.y - m_HudYOffset * (s_Screen.y / 1080.0f);
    const float s_Top = s_Bottom - s_H;
    const float s_Left = s_Cx - s_W * 0.5f;
    const float s_Right = s_Cx + s_W * 0.5f;

    const float s_Time = std::chrono::duration<float>(s_Now.time_since_epoch()).count();
    const float s_A = m_HudAlpha * m_HudOpacity;
    ImDrawList* s_Dl = ImGui::GetBackgroundDrawList();

    auto s_Col = [](float r, float g, float b, float a) {
        return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, std::clamp(a, 0.0f, 1.0f)));
    };

    // Fill color: gold normally, blends to the "low" color when nearly empty, pulses while recharging.
    const float s_Low = 1.0f - std::clamp(m_Meter / kLowLevel, 0.0f, 1.0f);
    float s_R = m_FillColor[0] + (m_LowColor[0] - m_FillColor[0]) * s_Low;
    float s_G = m_FillColor[1] + (m_LowColor[1] - m_FillColor[1]) * s_Low;
    float s_B = m_FillColor[2] + (m_LowColor[2] - m_FillColor[2]) * s_Low;
    float s_FillA = s_A;
    if (m_Blocked) {
        s_R = m_LowColor[0];
        s_G = m_LowColor[1];
        s_B = m_LowColor[2];
        s_FillA = s_A * (0.55f + 0.35f * std::sin(s_Time * 8.0f));
    }

    // Label above the bar.
    const char* s_Label = m_PsState.load() != PS_IDLE ? "POINT SHOOTING" : (m_Blocked ? "RECHARGING" : "INSTINCT");
    ImFont* s_Font = ImGui::GetFont();
    const float s_FontSize = std::max(10.0f, ImGui::GetFontSize() * 0.8f * s_Ui);
    const ImVec2 s_TextSize = s_Font->CalcTextSizeA(s_FontSize, FLT_MAX, 0.0f, s_Label);
    const ImVec2 s_TextPos(s_Cx - s_TextSize.x * 0.5f, s_Top - s_TextSize.y - 6.0f * s_Ui);
    s_Dl->AddText(s_Font, s_FontSize, ImVec2(s_TextPos.x + 1.0f, s_TextPos.y + 1.0f), IM_COL32(0, 0, 0, static_cast<int>(160 * s_A)), s_Label);
    s_Dl->AddText(s_Font, s_FontSize, s_TextPos, s_Col(1.0f, 0.93f, 0.75f, 0.85f * s_A), s_Label);

    // Dark backing, then the grey "empty" track.
    const float s_Inset = s_Thick * 2.0f;
    const float s_InnerL = s_Left + s_Inset;
    const float s_InnerR = s_Right - s_Inset;
    const float s_InnerT = s_Top + s_Inset;
    const float s_InnerB = s_Bottom - s_Inset;
    const float s_MidY = (s_InnerT + s_InnerB) * 0.5f;

    s_Dl->AddRectFilled(ImVec2(s_Left, s_Top), ImVec2(s_Right, s_Bottom), IM_COL32(10, 10, 12, static_cast<int>(170 * s_A)));
    s_Dl->AddRectFilled(ImVec2(s_InnerL, s_InnerT), ImVec2(s_InnerR, s_InnerB), IM_COL32(88, 88, 92, static_cast<int>(120 * s_A)));

    // Gold fill: darker at the start, brighter toward the tip.
    const float s_HeadX = s_InnerL + (s_InnerR - s_InnerL) * m_Meter;
    if (s_HeadX - s_InnerL > 0.5f) {
        const ImU32 s_Dim = s_Col(s_R * 0.50f, s_G * 0.50f, s_B * 0.50f, s_FillA);
        const ImU32 s_Bright = s_Col(s_R + (1.0f - s_R) * 0.35f, s_G + (1.0f - s_G) * 0.35f, s_B + (1.0f - s_B) * 0.35f, s_FillA);
        s_Dl->AddRectFilledMultiColor(ImVec2(s_InnerL, s_InnerT), ImVec2(s_HeadX, s_InnerB), s_Dim, s_Bright, s_Bright, s_Dim);

        // Thin highlight along the top of the fill.
        s_Dl->AddRectFilled(ImVec2(s_InnerL, s_InnerT), ImVec2(s_HeadX, s_InnerT + s_Thick),
                            IM_COL32(255, 255, 255, static_cast<int>(70 * s_A)));
    }

    // Sparkle particles: spawn at the tip while Instinct is in use, drift up and fade out.
    if (s_Active && m_Meter > 0.004f) {
        m_EmitAcc += s_Dt * 60.0f;
        while (m_EmitAcc >= 1.0f) {
            m_EmitAcc -= 1.0f;
            if (m_Particles.size() >= 160) break;

            Particle s_P;
            s_P.x = s_HeadX + (Rand01() - 0.5f) * 2.0f * s_Ui;
            s_P.y = s_InnerT + Rand01() * (s_InnerB - s_InnerT);
            s_P.vx = (Rand01() - 0.5f) * 70.0f * s_Ui;
            s_P.vy = -(12.0f + 55.0f * Rand01()) * s_Ui;
            s_P.maxLife = 0.35f + 0.65f * Rand01();
            s_P.life = s_P.maxLife;
            s_P.size = (0.8f + 1.8f * Rand01()) * std::max(1.0f, s_Ui);
            m_Particles.push_back(s_P);
        }
    } else {
        m_EmitAcc = 0.0f;
    }

    for (auto& s_P : m_Particles) {
        s_P.life -= s_Dt;
        s_P.x += s_P.vx * s_Dt;
        s_P.y += s_P.vy * s_Dt;
        s_P.vy += 40.0f * s_Ui * s_Dt; // slight pull down so the sparks arc
    }
    m_Particles.erase(std::remove_if(m_Particles.begin(), m_Particles.end(), [](const Particle& p) { return p.life <= 0.0f; }),
                      m_Particles.end());

    for (const auto& s_P : m_Particles) {
        const float s_K = s_P.life / s_P.maxLife;
        s_Dl->AddCircleFilled(ImVec2(s_P.x, s_P.y), s_P.size * (0.5f + 0.5f * s_K),
                              s_Col(s_R + (1.0f - s_R) * 0.55f, s_G + (1.0f - s_G) * 0.55f, s_B + (1.0f - s_B) * 0.55f, s_K * s_A), 6);
    }

    // Bright spark at the tip of the fill (stronger while Instinct is in use).
    if (m_Meter > 0.004f) {
        const float s_Pulse = s_Active ? 0.85f + 0.15f * std::sin(s_Time * 14.0f) : 0.5f;
        const float s_Rad = s_H * (s_Active ? 0.95f : 0.7f);

        for (int i = 0; i < 4; ++i) { // soft glow
            const float s_Rr = s_Rad * (2.6f - 0.6f * static_cast<float>(i));
            s_Dl->AddCircleFilled(ImVec2(s_HeadX, s_MidY), s_Rr, s_Col(s_R, s_G, s_B, (0.06f + 0.05f * static_cast<float>(i)) * s_Pulse * s_A), 20);
        }
        s_Dl->AddCircleFilled(ImVec2(s_HeadX, s_MidY), s_Rad * 0.5f, s_Col(1.0f, 0.97f, 0.88f, 0.95f * s_Pulse * s_A), 12);

        const float s_Len = s_Rad * (2.3f + (s_Active ? 0.6f * std::sin(s_Time * 10.0f) : 0.0f));
        const ImU32 s_Star = s_Col(1.0f, 0.97f, 0.88f, 0.8f * s_Pulse * s_A);
        s_Dl->AddLine(ImVec2(s_HeadX - s_Len, s_MidY), ImVec2(s_HeadX + s_Len, s_MidY), s_Star, s_Thick);
        s_Dl->AddLine(ImVec2(s_HeadX, s_MidY - s_Len * 0.8f), ImVec2(s_HeadX, s_MidY + s_Len * 0.8f), s_Star, s_Thick);
    }

    // Point shooting: one pip per possible target below the bar, filled for every tagged one.
    if (m_PsState.load() != PS_IDLE) {
        const int s_MaxTags = std::max(1, m_PsMaxTags);
        const int s_Tagged = m_PsTagCount.load();
        const float s_PipR = std::max(2.0f, 3.5f * s_Ui);
        const float s_Gap = s_PipR * 3.0f;
        const float s_TotalW = s_Gap * static_cast<float>(s_MaxTags - 1);
        const float s_PipY = s_Bottom + s_PipR * 2.6f;

        for (int i = 0; i < s_MaxTags; ++i) {
            const ImVec2 s_C(s_Cx - s_TotalW * 0.5f + s_Gap * static_cast<float>(i), s_PipY);
            if (i < s_Tagged) {
                s_Dl->AddCircleFilled(s_C, s_PipR * 1.9f, s_Col(1.0f, 0.78f, 0.28f, 0.18f * s_A), 14);
                s_Dl->AddCircleFilled(s_C, s_PipR, s_Col(1.0f, 0.86f, 0.45f, 0.95f * s_A), 12);
            } else {
                s_Dl->AddCircle(s_C, s_PipR, s_Col(1.0f, 0.9f, 0.65f, 0.35f * s_A), 12, s_Thick);
            }
        }
    }

    // Thin outline, a little brighter while Instinct is active.
    const float s_Border = s_Active ? 0.55f + 0.2f * std::sin(s_Time * 6.0f) : 0.35f;
    s_Dl->AddRect(ImVec2(s_Left, s_Top), ImVec2(s_Right, s_Bottom), s_Col(1.0f, 0.9f, 0.65f, s_Border * s_A), 0.0f, 0, s_Thick);
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

    // Outside a mission the game loop doesn't run, so never leave the game-level block switched on.
    if (!m_InPlay) {
        m_InputBlock = false;
    }

    // Action names are only recorded while the Debug section is open (set again below if it is).
    m_CaptureNames = false;

    if (!m_ShowUI) {
        m_Binding = false;
        m_BindingPs = false;
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
        ImGui::TextDisabled("Comma-separated, matches part of the game's action name. Find it in Debug below.");

        if (ImGui::CollapsingHeader("Debug")) {
            m_CaptureNames = true;

            ImGui::Text("Game updates seen: %u", m_FrameCount.load());
            ImGui::Text("Instinct key down:  Windows=%s  hook=%s", AsyncKeyDown(m_InstinctKey) ? "YES" : "no", HookKeyDown(m_InstinctKey) ? "YES" : "no");
            ImGui::Text("Hook events: keyboard=%u  mouse=%u  last key=0x%02X", g_KbEvents.load(), g_MouseEvents.load(), g_LastVk.load());
            ImGui::Text("Active=%s  Blocked=%s  Locked=%s  NeedRelease=%s", m_Active.load() ? "yes" : "no", m_Blocked ? "yes" : "no",
                        m_Locked ? "yes" : "no", m_NeedRelease ? "yes" : "no");
            ImGui::Text("Meter=%.2f  time scale=%.2f", m_Meter, m_CurrentScale);
            ImGui::Text("Game-level block: %s   blocked polls=%u", m_InputBlock.load() ? "ON" : "off", m_BlockedPolls.load());
            ImGui::TextDisabled("Updates stay at 0 outside a mission. Hold your Instinct key in a mission and watch these.");

            ImGui::Checkbox("Test: block matching actions now", &m_ForceBlockTest);
            ImGui::TextDisabled("With this on, Instinct should stop working in game. If it doesn't, pick another action name.");

            ImGui::InputText("Filter action names", m_NameFilter, sizeof(m_NameFilter));
            if (ImGui::Button("Clear list")) {
                std::lock_guard<std::mutex> s_Lock(m_NameMutex);
                m_SeenNames.clear();
            }
            ImGui::SameLine();
            ImGui::TextDisabled("Click a name to use it as the Instinct action");

            std::string s_Picked;
            if (ImGui::BeginChild("SeenActions", ImVec2(0, 150), true)) {
                std::lock_guard<std::mutex> s_Lock(m_NameMutex);
                for (const auto& s_SeenName : m_SeenNames) {
                    if (m_NameFilter[0] && !ContainsNoCase(s_SeenName.c_str(), [&]() {
                            std::string s_F = m_NameFilter;
                            std::transform(s_F.begin(), s_F.end(), s_F.begin(),
                                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                            return s_F;
                        }())) {
                        continue;
                    }
                    if (ImGui::Selectable(s_SeenName.c_str())) {
                        s_Picked = s_SeenName;
                    }
                }
            }
            ImGui::EndChild();

            if (!s_Picked.empty()) {
                std::snprintf(m_ActionPatterns, sizeof(m_ActionPatterns), "%s", s_Picked.c_str());
                RebuildPatterns();
                s_Changed = true;
            }
        } else {
            m_ForceBlockTest = false;
        }

        ImGui::Separator();

        // --- Point shooting ---
        if (ImGui::Checkbox("Point shooting (Absolution style)", &m_PsEnabled)) s_Changed = true;
        ImGui::BeginDisabled(!m_PsEnabled);
        ImGui::TextUnformatted("Point shooting key");
        if (DrawKeyBinder("psbind", m_PsKey, m_BindingPs, m_BindArmedPs)) s_Changed = true;
        ImGui::TextDisabled("Hold it and aim at enemies to tag them. Release it and they all get shot.");
        if (ImGui::SliderFloat("PS time scale", &m_PsScale, 0.05f, 0.95f, "%.2fx")) s_Changed = true;
        if (ImGui::SliderFloat("Meter per tagged target", &m_PsTagCost, 0.0f, 0.5f, "%.2f")) s_Changed = true;
        if (ImGui::SliderInt("Max targets", &m_PsMaxTags, 1, 12)) s_Changed = true;
        if (ImGui::SliderFloat("Time between shots", &m_PsShotDelay, 0.05f, 1.0f, "%.2f s")) s_Changed = true;
        if (ImGui::SliderFloat("Meter drain while aiming", &m_PsDrainMul, 0.0f, 2.0f, "%.2fx")) s_Changed = true;
        ImGui::EndDisabled();

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

        // --- HUD ---
        if (ImGui::Checkbox("Show Instinct meter", &m_HudEnabled)) s_Changed = true;
        ImGui::BeginDisabled(!m_HudEnabled);
        if (ImGui::Checkbox("Always visible (even when full)", &m_HudAlways)) s_Changed = true;
        if (ImGui::SliderFloat("Width", &m_HudWidth, 0.05f, 0.5f, "%.2f")) s_Changed = true;
        if (ImGui::SliderFloat("Height from bottom", &m_HudYOffset, 0.0f, 400.0f, "%.0f px")) s_Changed = true;
        if (ImGui::SliderFloat("Size", &m_HudScale, 0.5f, 3.0f, "%.2f")) s_Changed = true;
        if (ImGui::SliderFloat("Opacity", &m_HudOpacity, 0.1f, 1.0f, "%.2f")) s_Changed = true;
        if (ImGui::ColorEdit3("Meter color", m_FillColor)) s_Changed = true;
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
            ImGui::TextDisabled("Full path to a .wav or .mp3 file");
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
