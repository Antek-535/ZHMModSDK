#include "FocusForAll.h"

#include <Windows.h>
#include <mmsystem.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include <imgui.h>

#include "Glacier/ZGameLoopManager.h"
#include "Glacier/ZGameTime.h"

static constexpr char kSection[] = "FocusForAll";

// ---------------------------------------------------------------------------
// Sound synthesis: a short sine sweep, built in memory (no extra files needed).
// Enter = descending sweep (time "dropping"), exit = ascending sweep.
// ---------------------------------------------------------------------------
static std::vector<uint8_t> MakeSweepWav(float p_FreqStart, float p_FreqEnd, float p_Volume) {
    constexpr uint32_t kSampleRate = 22050;
    constexpr float kDuration = 0.22f;
    const uint32_t s_Samples = static_cast<uint32_t>(kSampleRate * kDuration);
    const uint32_t s_DataBytes = s_Samples * 2;

    std::vector<uint8_t> s_Wav(44 + s_DataBytes);
    auto* s_P = s_Wav.data();

    auto w32 = [&](size_t o, uint32_t v) { std::memcpy(s_P + o, &v, 4); };
    auto w16 = [&](size_t o, uint16_t v) { std::memcpy(s_P + o, &v, 2); };

    std::memcpy(s_P + 0, "RIFF", 4);
    w32(4, 36 + s_DataBytes);
    std::memcpy(s_P + 8, "WAVEfmt ", 8);
    w32(16, 16);
    w16(20, 1);                 // PCM
    w16(22, 1);                 // mono
    w32(24, kSampleRate);
    w32(28, kSampleRate * 2);   // byte rate
    w16(32, 2);                 // block align
    w16(34, 16);                // bits per sample
    std::memcpy(s_P + 36, "data", 4);
    w32(40, s_DataBytes);

    double s_Phase = 0.0;
    for (uint32_t i = 0; i < s_Samples; ++i) {
        const float s_T = static_cast<float>(i) / static_cast<float>(s_Samples); // 0..1
        const float s_Freq = p_FreqStart + (p_FreqEnd - p_FreqStart) * s_T;
        s_Phase += 2.0 * 3.14159265358979 * s_Freq / kSampleRate;

        const float s_Attack = std::min(1.0f, s_T / 0.05f);
        const float s_Decay = std::pow(1.0f - s_T, 1.5f);
        const float s_Sample = static_cast<float>(std::sin(s_Phase)) * s_Attack * s_Decay * p_Volume * 0.6f;

        const int16_t s_Pcm = static_cast<int16_t>(std::clamp(s_Sample, -1.0f, 1.0f) * 32767.0f);
        std::memcpy(s_P + 44 + i * 2, &s_Pcm, 2);
    }

    return s_Wav;
}

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
    RebuildSounds();

    const ZMemberDelegate<FocusForAll, void(const SGameUpdateEvent&)> s_Delegate(this, &FocusForAll::OnFrameUpdate);
    Globals::GameLoopManager->RegisterFrameUpdate(s_Delegate, 1, EUpdateMode::eUpdatePlayMode);
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

void FocusForAll::LoadSettings() {
    m_TriggerKey = static_cast<int>(GetSettingInt(kSection, "trigger_key", m_TriggerKey));
    m_Modifier = std::clamp(static_cast<int>(GetSettingInt(kSection, "modifier", m_Modifier)), 0, 3);
    m_ToggleMode = GetSettingBool(kSection, "toggle_mode", m_ToggleMode);
    m_FocusScale = std::clamp(static_cast<float>(GetSettingDouble(kSection, "focus_scale", m_FocusScale)), 0.05f, 0.95f);
    m_BlendSpeed = std::clamp(static_cast<float>(GetSettingDouble(kSection, "blend_speed", m_BlendSpeed)), 1.0f, 30.0f);
    m_SoundEnabled = GetSettingBool(kSection, "sound_enabled", m_SoundEnabled);
    m_Volume = std::clamp(static_cast<float>(GetSettingDouble(kSection, "volume", m_Volume)), 0.0f, 1.0f);
}

void FocusForAll::SaveSettings() {
    SetSettingInt(kSection, "trigger_key", m_TriggerKey);
    SetSettingInt(kSection, "modifier", m_Modifier);
    SetSettingBool(kSection, "toggle_mode", m_ToggleMode);
    SetSettingDouble(kSection, "focus_scale", m_FocusScale);
    SetSettingDouble(kSection, "blend_speed", m_BlendSpeed);
    SetSettingBool(kSection, "sound_enabled", m_SoundEnabled);
    SetSettingDouble(kSection, "volume", m_Volume);
}

// ---------------------------------------------------------------------------
// Sound
// ---------------------------------------------------------------------------

void FocusForAll::RebuildSounds() {
    PlaySoundW(nullptr, nullptr, 0); // make sure nothing is reading the old buffers
    m_EnterWav = MakeSweepWav(900.0f, 220.0f, m_Volume);
    m_ExitWav = MakeSweepWav(260.0f, 900.0f, m_Volume);
}

void FocusForAll::PlayCue(bool p_Enter) {
    if (!m_SoundEnabled || m_Volume <= 0.0f) {
        return;
    }

    const auto& s_Wav = p_Enter ? m_EnterWav : m_ExitWav;
    PlaySoundW(reinterpret_cast<LPCWSTR>(s_Wav.data()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

// ---------------------------------------------------------------------------
// Input helpers
// ---------------------------------------------------------------------------

bool FocusForAll::IsKeyDown(int p_Vk) {
    return p_Vk != 0 && (GetAsyncKeyState(p_Vk) & 0x8000) != 0;
}

const char* FocusForAll::ModifierName(int p_Modifier) {
    switch (p_Modifier) {
        case 1: return "Shift";
        case 2: return "Ctrl";
        case 3: return "Alt";
        default: return "None";
    }
}

std::string FocusForAll::KeyName(int p_Vk) {
    switch (p_Vk) {
        case VK_LBUTTON: return "Left Mouse";
        case VK_RBUTTON: return "Right Mouse";
        case VK_MBUTTON: return "Middle Mouse";
        case VK_XBUTTON1: return "Mouse 4";
        case VK_XBUTTON2: return "Mouse 5";
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
    const bool s_GameFocused = s_ForegroundPid == GetCurrentProcessId();

    // Is the configured combo currently held?
    int s_ModVk = 0;
    switch (m_Modifier) {
        case 1: s_ModVk = VK_SHIFT; break;
        case 2: s_ModVk = VK_CONTROL; break;
        case 3: s_ModVk = VK_MENU; break;
        default: break;
    }

    const bool s_ComboDown = s_GameFocused && !m_Binding && IsKeyDown(m_TriggerKey) && (s_ModVk == 0 || IsKeyDown(s_ModVk));

    bool s_Active;
    if (m_ToggleMode) {
        if (s_ComboDown && !m_PrevDown) {
            m_ToggleState = !m_ToggleState;
        }
        s_Active = m_ToggleState;
    } else {
        m_ToggleState = false;
        s_Active = s_ComboDown;
    }
    m_PrevDown = s_ComboDown;

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
// UI
// ---------------------------------------------------------------------------

void FocusForAll::OnDrawMenu() {
    if (ImGui::Button("FOCUS FOR ALL")) {
        m_ShowUI = !m_ShowUI;
    }
}

void FocusForAll::OnDrawUI(bool p_HasFocus) {
    if (!m_ShowUI) {
        if (m_Binding) {
            m_Binding = false;
        }
        return;
    }

    bool s_Changed = false;

    ImGui::SetNextWindowSize(ImVec2(380, 0), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Focus For All", &m_ShowUI)) {
        // --- Key binding ---
        ImGui::TextUnformatted("Trigger");

        if (m_Binding) {
            ImGui::Button("Press a key or mouse button... (Esc cancels)", ImVec2(-1, 0));

            // Wait until everything is released first, so the click on the button isn't captured.
            bool s_AnyDown = false;
            for (int vk = 1; vk < 255; ++vk) {
                if (vk >= VK_SHIFT && vk <= VK_MENU) continue; // generic modifiers; L/R variants are captured instead
                if (IsKeyDown(vk)) { s_AnyDown = true; break; }
            }

            if (!m_BindArmed) {
                m_BindArmed = !s_AnyDown;
            } else {
                for (int vk = 1; vk < 255; ++vk) {
                    if (vk >= VK_SHIFT && vk <= VK_MENU) continue;
                    if (!IsKeyDown(vk)) continue;

                    if (vk != VK_ESCAPE) {
                        m_TriggerKey = vk;
                        s_Changed = true;
                    }
                    m_Binding = false;
                    break;
                }
            }
        } else {
            const std::string s_Label = KeyName(m_TriggerKey) + "  (click to change)";
            if (ImGui::Button(s_Label.c_str(), ImVec2(-1, 0))) {
                m_Binding = true;
                m_BindArmed = false;
            }
        }

        if (ImGui::BeginCombo("Modifier", ModifierName(m_Modifier))) {
            for (int i = 0; i < 4; ++i) {
                if (ImGui::Selectable(ModifierName(i), m_Modifier == i)) {
                    m_Modifier = i;
                    s_Changed = true;
                }
            }
            ImGui::EndCombo();
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

        // --- Sound ---
        if (ImGui::Checkbox("Sound effect", &m_SoundEnabled)) s_Changed = true;

        ImGui::BeginDisabled(!m_SoundEnabled);
        ImGui::SliderFloat("Volume", &m_Volume, 0.0f, 1.0f, "%.2f");
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            RebuildSounds();
            s_Changed = true;
            PlayCue(true); // preview
        }
        ImGui::EndDisabled();
    }
    ImGui::End();

    if (s_Changed) {
        SaveSettings();
    }
}

DEFINE_ZHM_PLUGIN(FocusForAll)
