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
    void DrawOverlay();
    void RebuildSounds();
    void PlayCue(bool p_Enter);

    static bool IsKeyDown(int p_Vk);
    static std::string KeyName(int p_Vk);

    // --- Runtime state ---
    float m_CurrentScale = 1.0f;
    bool m_Touched = false;      // true while we are controlling the multiplier
    bool m_WasActive = false;    // for sound edge detection
    bool m_PrevDown = false;     // for toggle edge detection
    bool m_ToggleState = false;
    std::chrono::steady_clock::time_point m_LastFrame = std::chrono::steady_clock::now();

    // --- Settings (persisted) ---
    int m_SlowKey = 0x10;        // VK_SHIFT: the key that slows time
    int m_AimKey = 0x02;         // VK_RBUTTON: the game's aim button
    bool m_RequireAim = true;    // only slow time while the aim button is held
    bool m_ToggleMode = false;   // false = hold, true = press to toggle
    float m_FocusScale = 0.30f;  // time scale while focusing (1.0 = normal)
    float m_BlendSpeed = 6.0f;   // higher = snappier transition

    int m_OverlayStyle = 2;      // 0 = off, 1 = color tint, 2 = black & white
    float m_OverlayColor[3] = {0.35f, 0.65f, 1.0f};
    float m_OverlayOpacity = 0.15f;  // color tint max opacity
    float m_BwStrength = 0.55f;      // black & white max strength

    bool m_SoundEnabled = true;
    int m_SoundType = 0;         // index into the sound list in the .cpp
    float m_Volume = 0.5f;       // 0..1 (not applied to custom file)

    // --- UI state ---
    bool m_ShowUI = false;
    int m_BindingTarget = 0;     // 0 = none, 1 = slow key, 2 = aim key
    bool m_BindArmed = false;

    // --- Sounds (kept alive because PlaySound reads them asynchronously) ---
    std::vector<uint8_t> m_EnterWav;
    std::vector<uint8_t> m_ExitWav;
    std::wstring m_CustomSoundPath;
};

DECLARE_ZHM_PLUGIN(FocusForAll)
