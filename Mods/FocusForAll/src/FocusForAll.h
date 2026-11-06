#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "IPluginInterface.h"

class FocusForAll : public IPluginInterface {
public:
    ~FocusForAll() override;

    void OnEngineInitialized() override;
    void OnDrawMenu() override;
    void OnDrawUI(bool p_HasFocus) override;

private:
    void OnFrameUpdate(const SGameUpdateEvent& p_UpdateEvent);

    void LoadSettings();
    void SaveSettings();
    void RebuildSounds();
    void PlayCue(bool p_Enter);

    static bool IsKeyDown(int p_Vk);
    static std::string KeyName(int p_Vk);
    static const char* ModifierName(int p_Modifier);

    // --- Runtime state ---
    float m_CurrentScale = 1.0f;
    bool m_Touched = false;      // true while we are controlling the multiplier
    bool m_WasActive = false;    // for sound edge detection
    bool m_PrevDown = false;     // for toggle edge detection
    bool m_ToggleState = false;
    std::chrono::steady_clock::time_point m_LastFrame = std::chrono::steady_clock::now();

    // --- Settings (persisted) ---
    int m_TriggerKey = 0x02;     // VK_RBUTTON
    int m_Modifier = 1;          // 0 = none, 1 = Shift, 2 = Ctrl, 3 = Alt
    bool m_ToggleMode = false;   // false = hold, true = press to toggle
    float m_FocusScale = 0.30f;  // time scale while focusing (1.0 = normal)
    float m_BlendSpeed = 6.0f;   // higher = snappier transition
    bool m_SoundEnabled = true;
    float m_Volume = 0.5f;       // 0..1

    // --- UI state ---
    bool m_ShowUI = false;
    bool m_Binding = false;
    bool m_BindArmed = false;

    // --- Sounds (kept alive because PlaySound reads them asynchronously) ---
    std::vector<uint8_t> m_EnterWav;
    std::vector<uint8_t> m_ExitWav;
};

DECLARE_ZHM_PLUGIN(FocusForAll)
