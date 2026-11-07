#include "FocusForAll.h"

#include <Windows.h>
#include <mmsystem.h>
#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <memory>
#include <thread>
#include <vector>

#include <imgui.h>

#include "Glacier/ZGameLoopManager.h"
#include "Glacier/ZGameTime.h"

static constexpr char kSection[] = "FocusForAll";
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
// ---------------------------------------------------------------------------

std::atomic<bool> g_Block{false};
std::atomic<int> g_Key{0x10};
std::atomic<bool> g_Phys[256];
std::atomic<bool> g_HookOk{false};
std::atomic<bool> g_HookReady{false};
std::atomic<DWORD> g_HookThreadId{0};
DWORD g_Pid = 0;

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
        if (!(s_Kb->flags & LLKHF_INJECTED)) {
            const bool s_Down = p_W == WM_KEYDOWN || p_W == WM_SYSKEYDOWN;
            const bool s_Up = p_W == WM_KEYUP || p_W == WM_SYSKEYUP;
            if (s_Down || s_Up) {
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
        if (!(s_Ms->flags & LLMHF_INJECTED)) {
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
                g_Phys[s_Vk] = s_Down;
                if (s_Down && g_Block.load() && g_Key.load() == s_Vk && GameIsForeground()) {
                    return 1;
                }
            }
        }
    }
    return CallNextHookEx(nullptr, p_Code, p_W, p_L);
}

class InputGuard {
public:
    InputGuard() {
        g_Pid = GetCurrentProcessId();
        for (auto& s_Key : g_Phys) s_Key = false;
        m_Thread = std::thread([this] { Run(); });
        for (int i = 0; i < 200 && !g_HookReady.load(); ++i) Sleep(5); // wait (max ~1s) for the hooks to be installed
    }

    ~InputGuard() {
        g_Block = false;
        if (g_HookReady.load()) {
            PostThreadMessageW(g_HookThreadId.load(), WM_QUIT, 0, 0);
        }
        if (m_Thread.joinable()) m_Thread.join();
        g_HookOk = false;
        g_HookReady = false;
    }

private:
    void Run() {
        MSG s_Msg;
        PeekMessageW(&s_Msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE); // make sure this thread has a message queue
        g_HookThreadId = GetCurrentThreadId();

        HMODULE s_Module = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&KeyboardProc), &s_Module);

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
    }

    std::thread m_Thread;
};

std::unique_ptr<InputGuard> g_Guard;

bool PhysKeyDown(int p_Key) {
    for (int s_Vk : KeyVariants(p_Key)) {
        const bool s_Down = g_HookOk.load() ? g_Phys[s_Vk & 0xFF].load() : ((GetAsyncKeyState(s_Vk) & 0x8000) != 0);
        if (s_Down) return true;
    }
    return false;
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

} // namespace

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

FocusForAll::~FocusForAll() {
    const ZMemberDelegate<FocusForAll, void(const SGameUpdateEvent&)> s_Delegate(this, &FocusForAll::OnFrameUpdate);
    Globals::GameLoopManager->UnregisterFrameUpdate(s_Delegate, 1, EUpdateMode::eUpdatePlayMode);

    CloseSound();
    g_Guard.reset(); // removes the input hooks

    if (m_Touched && Globals::GameTimeManager) {
        Globals::GameTimeManager->m_fGameTimeMultiplier = 1.0f;
    }
}

void FocusForAll::OnEngineInitialized() {
    LoadSettings();

    m_ModDir = FindModuleDir();
    m_SoundDir = m_ModDir + L"InstinctSounds\\";
    std::error_code s_Ec;
    fs::create_directories(m_SoundDir, s_Ec);
    EnsurePresets();
    LoadSoundText();
    ScanSounds();

    g_Key = m_InstinctKey;
    g_Guard = std::make_unique<InputGuard>();

    const ZMemberDelegate<FocusForAll, void(const SGameUpdateEvent&)> s_Delegate(this, &FocusForAll::OnFrameUpdate);
    Globals::GameLoopManager->RegisterFrameUpdate(s_Delegate, 1, EUpdateMode::eUpdatePlayMode);
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

void FocusForAll::LoadSettings() {
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

    m_SoundEnabled = GetSettingBool(kSection, "sound_enabled", m_SoundEnabled);
    m_SoundSource = std::clamp(static_cast<int>(GetSettingInt(kSection, "sound_source", m_SoundSource)), 0, 1);
    m_SoundVolume = std::clamp(static_cast<float>(GetSettingDouble(kSection, "sound_volume", m_SoundVolume)), 0.0f, 1.0f);
    m_FadeOutOn = GetSettingBool(kSection, "fade_out_on", m_FadeOutOn);
    m_FadeOutMs = std::clamp(static_cast<float>(GetSettingDouble(kSection, "fade_out_ms", m_FadeOutMs)), 0.0f, 3000.0f);
    m_FadeInOn = GetSettingBool(kSection, "fade_in_on", m_FadeInOn);
    m_FadeInMs = std::clamp(static_cast<float>(GetSettingDouble(kSection, "fade_in_ms", m_FadeInMs)), 0.0f, 3000.0f);
}

void FocusForAll::SaveSettings() {
    SetSettingInt(kSection, "instinct_key", m_InstinctKey);
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
}

// The two text values (selected file name, custom path) live in a small text file next to the DLL.
void FocusForAll::LoadSoundText() {
    std::ifstream s_File(fs::path(m_ModDir + L"FocusForAll.sound.txt"));
    std::string s_Line;
    if (std::getline(s_File, s_Line)) {
        while (!s_Line.empty() && (s_Line.back() == '\r' || s_Line.back() == '\n')) s_Line.pop_back();
        m_PresetName = s_Line;
    }
    if (std::getline(s_File, s_Line)) {
        while (!s_Line.empty() && (s_Line.back() == '\r' || s_Line.back() == '\n')) s_Line.pop_back();
        std::snprintf(m_CustomPath, sizeof(m_CustomPath), "%s", s_Line.c_str());
    }
}

void FocusForAll::SaveSoundText() {
    std::ofstream s_File(fs::path(m_ModDir + L"FocusForAll.sound.txt"), std::ios::trunc);
    if (s_File) {
        s_File << m_PresetName << "\n" << m_CustomPath << "\n";
    }
}

// ---------------------------------------------------------------------------
// Sound (Windows MCI: wav and mp3, looping, volume fades)
// ---------------------------------------------------------------------------

void FocusForAll::EnsurePresets() {
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

void FocusForAll::ScanSounds() {
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

std::wstring FocusForAll::CurrentSoundPath() const {
    if (m_SoundSource == 1) {
        return Utf8ToWide(m_CustomPath);
    }
    if (m_PresetIndex >= 0 && m_PresetIndex < static_cast<int>(m_PresetFiles.size())) {
        return m_SoundDir + Utf8ToWide(m_PresetFiles[m_PresetIndex]);
    }
    return L"";
}

bool FocusForAll::OpenSound() {
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

void FocusForAll::CloseSound() {
    if (m_SndOpen) {
        mciSendStringW(L"stop instinctsnd", nullptr, 0, nullptr);
        mciSendStringW(L"close instinctsnd", nullptr, 0, nullptr);
    }
    m_SndOpen = false;
    m_SndPlaying = false;
    m_SndDir = 0;
    m_SndGain = 0.0f;
}

void FocusForAll::ApplySoundVolume() {
    if (!m_SndOpen) return;
    const int s_Vol = std::clamp(static_cast<int>(m_SndGain * m_SoundVolume * 1000.0f), 0, 1000);
    const std::wstring s_Cmd = L"setaudio instinctsnd volume to " + std::to_wstring(s_Vol);
    mciSendStringW(s_Cmd.c_str(), nullptr, 0, nullptr);
}

void FocusForAll::StartSound() {
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

void FocusForAll::StopSound() {
    if (!m_SndPlaying) return;

    if (m_FadeOutOn && m_FadeOutMs > 0.0f) {
        m_SndDir = -1;
    } else {
        CloseSound();
    }
}

// Runs every frame from the UI callback, so sound also stops cleanly when you leave the mission.
void FocusForAll::UpdateSound(float p_Dt, bool p_InPlay) {
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

bool FocusForAll::IsKeyDown(int p_Vk) {
    return p_Vk != 0 && (GetAsyncKeyState(p_Vk) & 0x8000) != 0;
}

bool FocusForAll::InstinctKeyDown() const {
    return PhysKeyDown(m_InstinctKey);
}

// Makes the game think the Instinct key was released (or, in toggle mode, taps it to switch Instinct off).
void FocusForAll::ForceReleaseInstinct() {
    const auto s_Variants = KeyVariants(m_InstinctKey);
    if (m_ToggleMode) {
        SendKey(s_Variants[0], false);
        SendKey(s_Variants[0], true);
        return;
    }

    for (int s_Vk : s_Variants) {
        const bool s_Down = g_HookOk.load() ? g_Phys[s_Vk & 0xFF].load() : ((GetAsyncKeyState(s_Vk) & 0x8000) != 0);
        if (s_Down) SendKey(s_Vk, true);
    }
}

std::string FocusForAll::KeyName(int p_Vk) {
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
// Game loop
// ---------------------------------------------------------------------------

void FocusForAll::OnFrameUpdate(const SGameUpdateEvent& p_UpdateEvent) {
    if (!Globals::GameTimeManager) {
        return;
    }

    // Real (unscaled) frame delta, so neither the meter nor the blend depend on the slowdown itself.
    using clock = std::chrono::steady_clock;
    const auto s_Now = clock::now();
    const float s_Dt = std::min(std::chrono::duration<float>(s_Now - m_LastFrame).count(), 0.1f);
    m_LastFrame = s_Now;
    m_LastUpdateTicks.store(s_Now.time_since_epoch().count());

    // Only react to keys when the game window is the active window.
    DWORD s_ForegroundPid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &s_ForegroundPid);
    const bool s_GameFocused = s_ForegroundPid == GetCurrentProcessId() && !m_Binding;
    const bool s_KeyDown = s_GameFocused && InstinctKeyDown();

    g_Key = m_InstinctKey;

    // Is Instinct usable right now? Not if the meter ran dry (until it recovers) or is below the start minimum.
    const bool s_WasActive = m_Active.load();
    m_Blocked = m_Locked || (!s_WasActive && m_Meter < kMinStart);

    // Block the key at the input level while unusable, so the game can't switch its own Instinct on either.
    g_Block = m_BlockEnabled && m_Blocked;

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
            ForceReleaseInstinct();
        }
    }

    m_Active.store(s_Want);

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
// HUD meter
// ---------------------------------------------------------------------------

void FocusForAll::DrawHud() {
    using clock = std::chrono::steady_clock;
    const auto s_Now = clock::now();
    const float s_Dt = std::min(std::chrono::duration<float>(s_Now - m_HudLast).count(), 0.1f);
    m_HudLast = s_Now;

    // The game loop only runs during play, so no recent update = menu / loading screen: hide the HUD.
    const int64_t s_SinceUpdate = s_Now.time_since_epoch().count() - m_LastUpdateTicks.load();
    const bool s_InPlay = std::chrono::duration_cast<std::chrono::milliseconds>(clock::duration(s_SinceUpdate)).count() < 250;
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
        return;
    }

    const ImVec2 s_Screen = ImGui::GetIO().DisplaySize;
    const float s_Ui = (s_Screen.y / 1080.0f) * m_HudScale;
    const float s_Thick = std::max(1.0f, std::round(s_Ui));
    const float s_W = s_Screen.x * m_HudWidth * m_HudScale;
    const float s_H = std::max(4.0f, 7.0f * s_Ui);
    const float s_Cx = s_Screen.x * 0.5f;
    const float s_Bottom = s_Screen.y - m_HudYOffset * (s_Screen.y / 1080.0f);
    const float s_Top = s_Bottom - s_H;
    const float s_Left = s_Cx - s_W * 0.5f;
    const float s_Right = s_Cx + s_W * 0.5f;

    const float s_Time = std::chrono::duration<float>(s_Now.time_since_epoch()).count();
    const float s_A = m_HudAlpha * m_HudOpacity;
    ImDrawList* s_Dl = ImGui::GetBackgroundDrawList();

    // Fill color: light normally, blends to the "low" color when nearly empty, pulses while blocked.
    const float s_Low = 1.0f - std::clamp(m_Meter / kLowLevel, 0.0f, 1.0f);
    ImVec4 s_Fill(
        m_FillColor[0] + (m_LowColor[0] - m_FillColor[0]) * s_Low,
        m_FillColor[1] + (m_LowColor[1] - m_FillColor[1]) * s_Low,
        m_FillColor[2] + (m_LowColor[2] - m_FillColor[2]) * s_Low,
        s_A);
    if (m_Blocked) {
        s_Fill = ImVec4(m_LowColor[0], m_LowColor[1], m_LowColor[2], s_A * (0.55f + 0.35f * std::sin(s_Time * 8.0f)));
    }

    // Label above the bar.
    const char* s_Label = m_Blocked ? "RECHARGING" : "INSTINCT";
    ImFont* s_Font = ImGui::GetFont();
    const float s_FontSize = std::max(10.0f, ImGui::GetFontSize() * 0.8f * s_Ui);
    const ImVec2 s_TextSize = s_Font->CalcTextSizeA(s_FontSize, FLT_MAX, 0.0f, s_Label);
    const ImVec2 s_TextPos(s_Cx - s_TextSize.x * 0.5f, s_Top - s_TextSize.y - 5.0f * s_Ui);
    s_Dl->AddText(s_Font, s_FontSize, ImVec2(s_TextPos.x + 1.0f, s_TextPos.y + 1.0f), IM_COL32(0, 0, 0, static_cast<int>(160 * s_A)), s_Label);
    s_Dl->AddText(s_Font, s_FontSize, s_TextPos, ImGui::ColorConvertFloat4ToU32(ImVec4(0.95f, 0.95f, 0.95f, 0.8f * s_A)), s_Label);

    // Dark translucent backing.
    s_Dl->AddRectFilled(ImVec2(s_Left, s_Top), ImVec2(s_Right, s_Bottom), IM_COL32(0, 0, 0, static_cast<int>(130 * s_A)));

    // Fill grows outward from the center.
    const float s_Inset = s_Thick * 2.0f;
    const float s_Half = std::max(0.0f, (s_W * 0.5f - s_Inset) * m_Meter);
    if (s_Half > 0.5f) {
        const ImU32 s_FillCol = ImGui::ColorConvertFloat4ToU32(s_Fill);
        s_Dl->AddRectFilled(ImVec2(s_Cx - s_Half, s_Top + s_Inset), ImVec2(s_Cx + s_Half, s_Bottom - s_Inset), s_FillCol);

        // Thin highlight along the top of the fill.
        s_Dl->AddRectFilled(ImVec2(s_Cx - s_Half, s_Top + s_Inset), ImVec2(s_Cx + s_Half, s_Top + s_Inset + s_Thick),
                            IM_COL32(255, 255, 255, static_cast<int>(70 * s_A)));
    }

    // Quarter ticks.
    for (int i = 1; i <= 3; ++i) {
        const float s_Off = (s_W * 0.5f - s_Inset) * (static_cast<float>(i) / 4.0f);
        const ImU32 s_TickCol = IM_COL32(0, 0, 0, static_cast<int>(150 * s_A));
        s_Dl->AddLine(ImVec2(s_Cx - s_Off, s_Top + s_Inset), ImVec2(s_Cx - s_Off, s_Bottom - s_Inset), s_TickCol, s_Thick);
        s_Dl->AddLine(ImVec2(s_Cx + s_Off, s_Top + s_Inset), ImVec2(s_Cx + s_Off, s_Bottom - s_Inset), s_TickCol, s_Thick);
    }

    // Thin outline, brighter and gently pulsing while Instinct is active.
    const float s_Border = s_Active ? 0.75f + 0.25f * std::sin(s_Time * 6.0f) : 0.45f;
    s_Dl->AddRect(ImVec2(s_Left, s_Top), ImVec2(s_Right, s_Bottom),
                  ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 1.0f, 1.0f, s_Border * s_A)), 0.0f, 0, s_Thick);
}

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------

void FocusForAll::OnDrawMenu() {
    if (ImGui::Button("FOCUS FOR ALL")) {
        m_ShowUI = !m_ShowUI;
    }
}

void FocusForAll::OnDrawUI(bool p_HasFocus) {
    DrawHud();
    UpdateSound(m_LastHudDt, m_InPlay);

    if (!m_ShowUI) {
        m_Binding = false;
        if (m_Testing) {
            m_Testing = false;
        }
        return;
    }

    bool s_Changed = false;

    ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Focus For All - Instinct", &m_ShowUI)) {
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

DEFINE_ZHM_PLUGIN(FocusForAll)
