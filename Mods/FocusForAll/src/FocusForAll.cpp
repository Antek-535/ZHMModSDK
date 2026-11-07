#include "FocusForAll.h"

#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <cfloat>
#include <cmath>
#include <cstdio>

#include <imgui.h>

#include "Glacier/ZGameLoopManager.h"
#include "Glacier/ZGameTime.h"

static constexpr char kSection[] = "FocusForAll";
static constexpr float kUnlockLevel = 0.25f; // after running dry, the meter must refill to this before Instinct slows time again
static constexpr float kLowLevel = 0.25f;    // below this the meter turns to the "low" color

FocusForAll::~FocusForAll() {
    const ZMemberDelegate<FocusForAll, void(const SGameUpdateEvent&)> s_Delegate(this, &FocusForAll::OnFrameUpdate);
    Globals::GameLoopManager->UnregisterFrameUpdate(s_Delegate, 1, EUpdateMode::eUpdatePlayMode);

    if (m_Touched && Globals::GameTimeManager) {
        Globals::GameTimeManager->m_fGameTimeMultiplier = 1.0f;
    }
}

void FocusForAll::OnEngineInitialized() {
    LoadSettings();

    const ZMemberDelegate<FocusForAll, void(const SGameUpdateEvent&)> s_Delegate(this, &FocusForAll::OnFrameUpdate);
    Globals::GameLoopManager->RegisterFrameUpdate(s_Delegate, 1, EUpdateMode::eUpdatePlayMode);
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

void FocusForAll::LoadSettings() {
    m_InstinctKey = static_cast<int>(GetSettingInt(kSection, "instinct_key", m_InstinctKey));
    m_ToggleMode = GetSettingBool(kSection, "toggle_mode", m_ToggleMode);
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
}

void FocusForAll::SaveSettings() {
    SetSettingInt(kSection, "instinct_key", m_InstinctKey);
    SetSettingBool(kSection, "toggle_mode", m_ToggleMode);
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
    const bool s_KeyDown = s_GameFocused && IsKeyDown(m_InstinctKey);

    // Does the player want Instinct right now?
    bool s_Want;
    if (m_ToggleMode) {
        if (s_KeyDown && !m_PrevDown && !m_Locked) {
            m_ToggleState = !m_ToggleState;
        }
        s_Want = m_ToggleState;
    } else {
        m_ToggleState = false;
        s_Want = s_KeyDown;
    }
    m_PrevDown = s_KeyDown;

    // Meter: drains while active, recharges (after a short delay) otherwise.
    if (m_Locked) {
        s_Want = false;
        m_ToggleState = false;
    }

    if (s_Want && m_Meter <= 0.0f) {
        m_Locked = true; // ran dry
        m_ToggleState = false;
        s_Want = false;
    }

    m_Active = s_Want;

    if (m_Active) {
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

    if (m_Active || m_Meter < 0.999f) {
        m_HoldVisible = 1.5f;
    } else {
        m_HoldVisible = std::max(0.0f, m_HoldVisible - s_Dt);
    }

    // Time scale.
    const float s_Target = m_Active ? m_FocusScale : 1.0f;
    const float s_T = std::clamp(m_BlendSpeed * s_Dt, 0.0f, 1.0f);
    m_CurrentScale += (s_Target - m_CurrentScale) * s_T;

    if (m_Active || std::abs(m_CurrentScale - 1.0f) > 0.002f) {
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
    const bool s_Preview = m_ShowUI; // while the settings window is open, always show the meter so it can be positioned

    float s_TargetAlpha = 0.0f;
    if (m_HudEnabled && (s_InPlay || s_Preview)) {
        if (s_Preview || m_HudAlways || m_Active || m_Locked || m_HoldVisible > 0.0f) {
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

    // Fill color: light normally, blends to the "low" color when nearly empty, pulses while locked.
    const float s_Low = 1.0f - std::clamp(m_Meter / kLowLevel, 0.0f, 1.0f);
    ImVec4 s_Fill(
        m_FillColor[0] + (m_LowColor[0] - m_FillColor[0]) * s_Low,
        m_FillColor[1] + (m_LowColor[1] - m_FillColor[1]) * s_Low,
        m_FillColor[2] + (m_LowColor[2] - m_FillColor[2]) * s_Low,
        s_A);
    if (m_Locked) {
        s_Fill = ImVec4(m_LowColor[0], m_LowColor[1], m_LowColor[2], s_A * (0.55f + 0.35f * std::sin(s_Time * 8.0f)));
    }

    // Label above the bar.
    const char* s_Label = m_Locked ? "RECHARGING" : "INSTINCT";
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
    const float s_Border = m_Active ? 0.75f + 0.25f * std::sin(s_Time * 6.0f) : 0.45f;
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

    if (!m_ShowUI) {
        m_Binding = false;
        return;
    }

    bool s_Changed = false;

    ImGui::SetNextWindowSize(ImVec2(400, 0), ImGuiCond_FirstUseEver);
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
    }
    ImGui::End();

    if (s_Changed) {
        SaveSettings();
    }
}

DEFINE_ZHM_PLUGIN(FocusForAll)
