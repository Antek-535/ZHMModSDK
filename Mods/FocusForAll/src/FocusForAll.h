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
    void DrawVignette(float p_Strength);
    void RebuildSounds();
    bool ReadHeroFlags(uint64_t (&p_Out)[3]) const;
    bool LearnedStateMatches() const;
    int LearnedBitCount() const;
    void TakeSample(bool p_Aiming);
    void SetToast(const std::string& p_Text, float p_Seconds);
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
    int m_AimMode = 1;           // 0 = always, 1 = aim button, 2 = aim button + learned weapon state
    bool m_ToggleMode = false;   // false = hold, true = press to toggle
    float m_FocusScale = 0.30f;  // time scale while focusing (1.0 = normal)
    float m_BlendSpeed = 6.0f;   // higher = snappier transition

    int m_OverlayStyle = 2;      // 0 = off, 1 = color tint, 2 = black & white
    float m_OverlayColor[3] = {0.35f, 0.65f, 1.0f};
    float m_OverlayOpacity = 0.15f;  // color tint max opacity
    float m_BwStrength = 0.55f;      // black & white max strength

    bool m_VigEnabled = true;
    float m_VigColor[3] = {0.20f, 0.45f, 1.0f};
    float m_VigOpacity = 0.60f;
    float m_VigSize = 0.28f;     // band thickness as a fraction of the screen size

    // Weapon calibration: bits that are set in every "aiming with weapon" sample
    // and in none of the "not aiming / no weapon" samples.
    uint64_t m_OnAnd[3] = {~0ull, ~0ull, ~0ull};
    uint64_t m_OffOr[3] = {0ull, 0ull, 0ull};
    int m_OnCount = 0;
    int m_OffCount = 0;

    bool m_SoundEnabled = true;
    int m_SoundType = 0;         // index into the sound list in the .cpp
    float m_Volume = 0.5f;       // 0..1 (not applied to custom file)

    // --- UI state ---
    bool m_ShowUI = false;
    int m_BindingTarget = 0;     // 0 = none, 1 = slow key, 2 = aim key
    bool m_BindArmed = false;
    int m_PendingSample = 0;     // 0 = none, 1 = aiming sample, 2 = not-aiming sample
    std::chrono::steady_clock::time_point m_SampleTime;
    std::string m_Toast;
    std::chrono::steady_clock::time_point m_ToastUntil;

    // --- Sounds (kept alive because PlaySound reads them asynchronously) ---
    std::vector<uint8_t> m_EnterWav;
    std::vector<uint8_t> m_ExitWav;
    std::wstring m_CustomSoundPath;
};

DECLARE_ZHM_PLUGIN(FocusForAll)
