#include "FocusForAll.h"

#include <Windows.h>
#include <mmsystem.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <imgui.h>

#include "Glacier/ZGameLoopManager.h"
#include "Glacier/ZGameTime.h"

static constexpr char kSection[] = "FocusForAll";

// ---------------------------------------------------------------------------
// Sounds. All built in memory, no files required (except the optional custom one).
// ---------------------------------------------------------------------------

static const char* const kSoundNames[] = {
    "Sweep", "Heartbeat", "Whoosh", "Blips", "Deep boom", "Custom file (FocusForAll.wav)"
};
static constexpr int kSoundCount = 6;
static constexpr int kSoundCustom = 5;

namespace {

constexpr uint32_t kRate = 22050;
constexpr float kPi = 3.14159265358979f;
using Samples = std::vector<float>;

Samples SoundSweep(bool p_Enter) {
    const uint32_t n = static_cast<uint32_t>(kRate * 0.22f);
    const float f0 = p_Enter ? 900.0f : 260.0f;
    const float f1 = p_Enter ? 220.0f : 900.0f;
    Samples s(n);
    double phase = 0.0;
    for (uint32_t i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / n;
        phase += 2.0 * kPi * (f0 + (f1 - f0) * t) / kRate;
        s[i] = static_cast<float>(std::sin(phase)) * std::min(1.0f, t / 0.05f) * std::pow(1.0f - t, 1.5f);
    }
    return s;
}

Samples SoundHeartbeat(bool p_Enter) {
    const float dur = p_Enter ? 0.45f : 0.25f;
    const uint32_t n = static_cast<uint32_t>(kRate * dur);
    Samples s(n, 0.0f);
    auto thump = [&](float p_Start, float p_Amp) {
        for (uint32_t i = 0; i < n; ++i) {
            const float t = static_cast<float>(i) / kRate - p_Start;
            if (t < 0.0f) continue;
            const float env = std::exp(-t * 14.0f) * std::min(1.0f, t / 0.004f);
            s[i] += p_Amp * env * (std::sin(2.0f * kPi * 55.0f * t) + 0.5f * std::sin(2.0f * kPi * 110.0f * t));
        }
    };
    if (p_Enter) { thump(0.0f, 1.0f); thump(0.17f, 0.7f); }
    else { thump(0.0f, 0.6f); }
    return s;
}

Samples SoundWhoosh(bool p_Enter) {
    const uint32_t n = static_cast<uint32_t>(kRate * 0.35f);
    Samples s(n);
    uint32_t rng = 12345u;
    float y = 0.0f;
    for (uint32_t i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / n;
        rng = rng * 1664525u + 1013904223u;
        const float noise = (static_cast<float>(rng >> 8) / 8388608.0f) - 1.0f;
        const float a = p_Enter ? (0.5f - 0.47f * t) : (0.03f + 0.47f * t); // low-pass sweep
        y += a * (noise - y);
        s[i] = y * 2.2f * std::sin(kPi * t);
    }
    return s;
}

Samples SoundBlips(bool p_Enter) {
    const float f1 = p_Enter ? 520.0f : 390.0f;
    const float f2 = p_Enter ? 390.0f : 520.0f;
    const uint32_t seg = static_cast<uint32_t>(kRate * 0.07f);
    const uint32_t gap = static_cast<uint32_t>(kRate * 0.03f);
    Samples s(seg * 2 + gap, 0.0f);
    for (int b = 0; b < 2; ++b) {
        const float f = b == 0 ? f1 : f2;
        const uint32_t off = b * (seg + gap);
        for (uint32_t i = 0; i < seg; ++i) {
            const float t = static_cast<float>(i) / seg;
            s[off + i] = std::sin(2.0f * kPi * f * i / kRate) * std::min(1.0f, t / 0.1f) * (1.0f - t);
        }
    }
    return s;
}

Samples SoundBoom(bool p_Enter) {
    if (p_Enter) {
        const uint32_t n = static_cast<uint32_t>(kRate * 0.4f);
        Samples s(n);
        double phase = 0.0;
        for (uint32_t i = 0; i < n; ++i) {
            const float t = static_cast<float>(i) / n;
            phase += 2.0 * kPi * (90.0f - 55.0f * t) / kRate;
            s[i] = static_cast<float>(std::sin(phase)) * std::min(1.0f, t / 0.02f) * std::exp(-t * 6.0f);
        }
        return s;
    }
    const uint32_t n = static_cast<uint32_t>(kRate * 0.05f);
    Samples s(n);
    for (uint32_t i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / n;
        s[i] = std::sin(2.0f * kPi * 1200.0f * i / kRate) * std::exp(-t * 8.0f) * 0.7f;
    }
    return s;
}

std::vector<uint8_t> ToWav(const Samples& p_Samples, float p_Volume) {
    const uint32_t n = static_cast<uint32_t>(p_Samples.size());
    const uint32_t dataBytes = n * 2;
    std::vector<uint8_t> wav(44 + dataBytes);
    auto* p = wav.data();

    auto w32 = [&](size_t o, uint32_t v) { std::memcpy(p + o, &v, 4); };
    auto w16 = [&](size_t o, uint16_t v) { std::memcpy(p + o, &v, 2); };

    std::memcpy(p + 0, "RIFF", 4);
    w32(4, 36 + dataBytes);
    std::memcpy(p + 8, "WAVEfmt ", 8);
    w32(16, 16);
    w16(20, 1);
    w16(22, 1);
    w32(24, kRate);
    w32(28, kRate * 2);
    w16(32, 2);
    w16(34, 16);
    std::memcpy(p + 36, "data", 4);
    w32(40, dataBytes);

    for (uint32_t i = 0; i < n; ++i) {
        const float v = std::clamp(p_Samples[i] * p_Volume * 0.6f, -1.0f, 1.0f);
        const int16_t pcm = static_cast<int16_t>(v * 32767.0f);
        std::memcpy(p + 44 + i * 2, &pcm, 2);
    }
    return wav;
}

Samples MakeSound(int p_Type, bool p_Enter) {
    switch (p_Type) {
        case 1: return SoundHeartbeat(p_Enter);
        case 2: return SoundWhoosh(p_Enter);
        case 3: return SoundBlips(p_Enter);
        case 4: return SoundBoom(p_Enter);
        default: return SoundSweep(p_Enter);
    }
}

std::wstring FindCustomSoundPath() {
    HMODULE s_Module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&FindCustomSoundPath), &s_Module);
    wchar_t s_Path[MAX_PATH] = {};
    GetModuleFileNameW(s_Module, s_Path, MAX_PATH);
    std::wstring s_Dir = s_Path;
    const size_t s_Slash = s_Dir.find_last_of(L"\\/");
    s_Dir = s_Dir.substr(0, s_Slash == std::wstring::npos ? 0 : s_Slash + 1);
    return s_Dir + L"FocusForAll.wav";
}

} // namespace

// ---------------------------------------------------------------------------

FocusForAll::~FocusForAll() {
    const ZMemberDelegate<FocusForAll, void(const SGameUpdateEvent&)> s_Delegate(this, &FocusForAll::OnFrameUpdate);
    Globals::GameLoopManager->UnregisterFrameUpdate(s_Delegate, 1, EUpdateMode::eUpdatePlayMode);

    PlaySoundW(nullptr, nullptr, 0); // stop any sound still reading our buffers

    if (m_Touched && Globals::GameTimeManager) {
        Globals::GameTimeManager->m_fGameTimeMultiplier = 1.0f;
    }
}

void FocusForAll::OnEngineInitialized() {
    LoadSettings();
    m_CustomSoundPath = FindCustomSoundPath();
    RebuildSounds();

    const ZMemberDelegate<FocusForAll, void(const SGameUpdateEvent&)> s_Delegate(this, &FocusForAll::OnFrameUpdate);
    Globals::GameLoopManager->RegisterFrameUpdate(s_Delegate, 1, EUpdateMode::eUpdatePlayMode);
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

void FocusForAll::LoadSettings() {
    m_SlowKey = static_cast<int>(GetSettingInt(kSection, "slow_key", m_SlowKey));
    m_AimKey = static_cast<int>(GetSettingInt(kSection, "aim_key", m_AimKey));
    m_RequireAim = GetSettingBool(kSection, "require_aim", m_RequireAim);
    m_ToggleMode = GetSettingBool(kSection, "toggle_mode", m_ToggleMode);
    m_FocusScale = std::clamp(static_cast<float>(GetSettingDouble(kSection, "focus_scale", m_FocusScale)), 0.05f, 0.95f);
    m_BlendSpeed = std::clamp(static_cast<float>(GetSettingDouble(kSection, "blend_speed", m_BlendSpeed)), 1.0f, 30.0f);

    m_OverlayStyle = std::clamp(static_cast<int>(GetSettingInt(kSection, "overlay_style", m_OverlayStyle)), 0, 2);
    m_OverlayColor[0] = std::clamp(static_cast<float>(GetSettingDouble(kSection, "overlay_r", m_OverlayColor[0])), 0.0f, 1.0f);
    m_OverlayColor[1] = std::clamp(static_cast<float>(GetSettingDouble(kSection, "overlay_g", m_OverlayColor[1])), 0.0f, 1.0f);
    m_OverlayColor[2] = std::clamp(static_cast<float>(GetSettingDouble(kSection, "overlay_b", m_OverlayColor[2])), 0.0f, 1.0f);
    m_OverlayOpacity = std::clamp(static_cast<float>(GetSettingDouble(kSection, "overlay_opacity", m_OverlayOpacity)), 0.0f, 0.6f);
    m_BwStrength = std::clamp(static_cast<float>(GetSettingDouble(kSection, "bw_strength", m_BwStrength)), 0.0f, 0.9f);

    m_SoundEnabled = GetSettingBool(kSection, "sound_enabled", m_SoundEnabled);
    m_SoundType = std::clamp(static_cast<int>(GetSettingInt(kSection, "sound_type", m_SoundType)), 0, kSoundCount - 1);
    m_Volume = std::clamp(static_cast<float>(GetSettingDouble(kSection, "volume", m_Volume)), 0.0f, 1.0f);
}

void FocusForAll::SaveSettings() {
    SetSettingInt(kSection, "slow_key", m_SlowKey);
    SetSettingInt(kSection, "aim_key", m_AimKey);
    SetSettingBool(kSection, "require_aim", m_RequireAim);
    SetSettingBool(kSection, "toggle_mode", m_ToggleMode);
    SetSettingDouble(kSection, "focus_scale", m_FocusScale);
    SetSettingDouble(kSection, "blend_speed", m_BlendSpeed);

    SetSettingInt(kSection, "overlay_style", m_OverlayStyle);
    SetSettingDouble(kSection, "overlay_r", m_OverlayColor[0]);
    SetSettingDouble(kSection, "overlay_g", m_OverlayColor[1]);
    SetSettingDouble(kSection, "overlay_b", m_OverlayColor[2]);
    SetSettingDouble(kSection, "overlay_opacity", m_OverlayOpacity);
    SetSettingDouble(kSection, "bw_strength", m_BwStrength);

    SetSettingBool(kSection, "sound_enabled", m_SoundEnabled);
    SetSettingInt(kSection, "sound_type", m_SoundType);
    SetSettingDouble(kSection, "volume", m_Volume);
}

// ---------------------------------------------------------------------------
// Sound
// ---------------------------------------------------------------------------

void FocusForAll::RebuildSounds() {
    PlaySoundW(nullptr, nullptr, 0); // make sure nothing is reading the old buffers
    if (m_SoundType == kSoundCustom) {
        m_EnterWav.clear();
        m_ExitWav.clear();
        return;
    }
    m_EnterWav = ToWav(MakeSound(m_SoundType, true), m_Volume);
    m_ExitWav = ToWav(MakeSound(m_SoundType, false), m_Volume);
}

void FocusForAll::PlayCue(bool p_Enter) {
    if (!m_SoundEnabled) {
        return;
    }

    if (m_SoundType == kSoundCustom) {
        if (p_Enter) { // custom file plays when slow-mo starts only
            PlaySoundW(m_CustomSoundPath.c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
        }
        return;
    }

    if (m_Volume <= 0.0f) {
        return;
    }

    const auto& s_Wav = p_Enter ? m_EnterWav : m_ExitWav;
    if (!s_Wav.empty()) {
        PlaySoundW(reinterpret_cast<LPCWSTR>(s_Wav.data()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
    }
}

// ---------------------------------------------------------------------------
// Input helpers
// ---------------------------------------------------------------------------

bool FocusForAll::IsKeyDown(int p_Vk) {
    return p_Vk != 0 && (GetAsyncKeyState(p_Vk) & 0x8000) != 0;
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

    // Real (unscaled) frame delta, so the blend speed doesn't depend on the slowdown itself.
    using clock = std::chrono::steady_clock;
    const auto s_Now = clock::now();
    const float s_Dt = std::min(std::chrono::duration<float>(s_Now - m_LastFrame).count(), 0.1f);
    m_LastFrame = s_Now;

    // Only react to keys when the game window is the active window.
    DWORD s_ForegroundPid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &s_ForegroundPid);
    const bool s_GameFocused = s_ForegroundPid == GetCurrentProcessId() && m_BindingTarget == 0;

    // "Aiming" = the game's aim button is held (see notes: this does not check for an equipped weapon).
    const bool s_Aiming = !m_RequireAim || IsKeyDown(m_AimKey);
    const bool s_SlowKeyDown = s_GameFocused && IsKeyDown(m_SlowKey);

    bool s_Active;
    if (m_ToggleMode) {
        if (s_SlowKeyDown && !m_PrevDown && s_Aiming) {
            m_ToggleState = !m_ToggleState;
        }
        if (!s_Aiming) {
            m_ToggleState = false; // lowering the weapon ends slow-mo
        }
        s_Active = m_ToggleState;
    } else {
        m_ToggleState = false;
        s_Active = s_SlowKeyDown && s_Aiming;
    }
    m_PrevDown = s_SlowKeyDown;

    // Sound cue on state change.
    if (s_Active != m_WasActive) {
        PlayCue(s_Active);
        m_WasActive = s_Active;
    }

    const float s_Target = s_Active ? m_FocusScale : 1.0f;
    const float s_T = std::clamp(m_BlendSpeed * s_Dt, 0.0f, 1.0f);
    m_CurrentScale += (s_Target - m_CurrentScale) * s_T;

    if (s_Active || std::abs(m_CurrentScale - 1.0f) > 0.002f) {
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
// Overlay
// ---------------------------------------------------------------------------

void FocusForAll::DrawOverlay() {
    if (m_OverlayStyle == 0 || !m_Touched) {
        return;
    }

    // 0 at normal speed, 1 at full slowdown, so the effect fades with the time blend.
    const float s_Range = std::max(1.0f - m_FocusScale, 0.001f);
    const float s_Strength = std::clamp((1.0f - m_CurrentScale) / s_Range, 0.0f, 1.0f);

    ImVec4 s_Color;
    if (m_OverlayStyle == 2) {
        s_Color = ImVec4(0.5f, 0.5f, 0.5f, s_Strength * m_BwStrength);
    } else {
        s_Color = ImVec4(m_OverlayColor[0], m_OverlayColor[1], m_OverlayColor[2], s_Strength * m_OverlayOpacity);
    }

    if (s_Color.w <= 0.001f) {
        return;
    }

    ImGui::GetBackgroundDrawList()->AddRectFilled(
        ImVec2(0.0f, 0.0f), ImGui::GetIO().DisplaySize, ImGui::ColorConvertFloat4ToU32(s_Color));
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
    DrawOverlay();

    if (!m_ShowUI) {
        m_BindingTarget = 0;
        return;
    }

    bool s_Changed = false;
    bool s_SoundChanged = false;

    ImGui::SetNextWindowSize(ImVec2(400, 0), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Focus For All", &m_ShowUI)) {
        // --- Key bindings ---
        auto s_KeyButton = [&](const char* p_Label, int p_Key, int p_Id) {
            ImGui::TextUnformatted(p_Label);
            ImGui::PushID(p_Id);
            if (m_BindingTarget == p_Id) {
                ImGui::Button("Press a key or mouse button... (Esc cancels)", ImVec2(-1, 0));
            } else {
                const std::string s_Text = KeyName(p_Key) + "  (click to change)";
                if (ImGui::Button(s_Text.c_str(), ImVec2(-1, 0))) {
                    m_BindingTarget = p_Id;
                    m_BindArmed = false;
                }
            }
            ImGui::PopID();
        };

        s_KeyButton("Slow-down key", m_SlowKey, 1);

        if (ImGui::Checkbox("Only works while aiming", &m_RequireAim)) s_Changed = true;
        ImGui::BeginDisabled(!m_RequireAim);
        s_KeyButton("Aim button (match your in-game aim bind)", m_AimKey, 2);
        ImGui::EndDisabled();

        // Capture the next key press for whichever binding is active.
        if (m_BindingTarget != 0) {
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
                        (m_BindingTarget == 1 ? m_SlowKey : m_AimKey) = vk;
                        s_Changed = true;
                    }
                    m_BindingTarget = 0;
                    break;
                }
            }
        }

        int s_Mode = m_ToggleMode ? 1 : 0;
        if (ImGui::RadioButton("Hold", &s_Mode, 0)) { m_ToggleMode = false; s_Changed = true; }
        ImGui::SameLine();
        if (ImGui::RadioButton("Toggle", &s_Mode, 1)) { m_ToggleMode = true; s_Changed = true; }

        ImGui::Separator();

        // --- Slowdown ---
        if (ImGui::SliderFloat("Time scale", &m_FocusScale, 0.05f, 0.95f, "%.2fx")) s_Changed = true;
        if (ImGui::SliderFloat("Transition speed", &m_BlendSpeed, 1.0f, 30.0f, "%.1f")) s_Changed = true;

        ImGui::Separator();

        // --- Screen overlay ---
        static const char* const s_StyleNames[] = {"Off", "Color tint", "Black & white"};
        if (ImGui::Combo("Screen effect", &m_OverlayStyle, s_StyleNames, 3)) s_Changed = true;

        if (m_OverlayStyle == 1) {
            if (ImGui::ColorEdit3("Tint color", m_OverlayColor)) s_Changed = true;
            if (ImGui::SliderFloat("Tint opacity", &m_OverlayOpacity, 0.0f, 0.6f, "%.2f")) s_Changed = true;
        } else if (m_OverlayStyle == 2) {
            if (ImGui::SliderFloat("B&W strength", &m_BwStrength, 0.0f, 0.9f, "%.2f")) s_Changed = true;
        }

        ImGui::Separator();

        // --- Sound ---
        if (ImGui::Checkbox("Sound effect", &m_SoundEnabled)) s_Changed = true;

        ImGui::BeginDisabled(!m_SoundEnabled);
        if (ImGui::Combo("Sound", &m_SoundType, kSoundNames, kSoundCount)) {
            s_Changed = true;
            s_SoundChanged = true;
        }
        if (m_SoundType == kSoundCustom) {
            const bool s_Exists = GetFileAttributesW(m_CustomSoundPath.c_str()) != INVALID_FILE_ATTRIBUTES;
            ImGui::TextWrapped(s_Exists ? "Using FocusForAll.wav from the mods folder (plays when slow-mo starts)."
                                        : "Put a file named FocusForAll.wav next to FocusForAll.dll (mods folder).");
        } else {
            ImGui::SliderFloat("Volume", &m_Volume, 0.0f, 1.0f, "%.2f");
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                s_Changed = true;
                s_SoundChanged = true;
            }
        }
        ImGui::EndDisabled();
    }
    ImGui::End();

    if (s_SoundChanged) {
        RebuildSounds();
        PlayCue(true); // preview
    }

    if (s_Changed) {
        SaveSettings();
    }
}

DEFINE_ZHM_PLUGIN(FocusForAll)
