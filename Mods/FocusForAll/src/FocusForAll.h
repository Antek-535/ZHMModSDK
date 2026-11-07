#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "IPluginInterface.h"

// Ultimate Instinct: Instinct slows time and drains a meter. When the meter is too low Instinct is
// blocked entirely, and the looping Instinct sound only plays while Instinct is really usable.
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
    void DrawHud();

    // Sound (Windows MCI, same approach as the old InstinctSounds mod).
    void LoadSoundText();
    void SaveSoundText();
    void ScanSounds();
    void EnsurePresets();
    std::wstring CurrentSoundPath() const;
    bool OpenSound();
    void CloseSound();
    void ApplySoundVolume();
    void StartSound();
    void StopSound();
    void UpdateSound(float p_Dt, bool p_InPlay);

    bool InstinctKeyDown() const;
    void ForceReleaseInstinct();

    static bool IsKeyDown(int p_Vk);
    static std::string KeyName(int p_Vk);

    // --- Runtime state ---
    float m_CurrentScale = 1.0f;     // current (blended) time scale
    bool m_Touched = false;          // true while we are controlling the multiplier
    bool m_PrevDown = false;         // for toggle edge detection
    bool m_ToggleState = false;
    std::atomic<bool> m_Active{false}; // Instinct slow-mo currently active (meter available)
    bool m_Locked = false;           // meter ran dry: wait for it to recharge a bit
    bool m_Blocked = false;          // Instinct is currently unusable (locked, or meter below the start minimum)
    bool m_NeedRelease = false;      // key must be released before Instinct can start again
    float m_Meter = 1.0f;            // 0..1
    float m_RechargeWait = 0.0f;     // seconds left before recharging starts
    float m_HoldVisible = 0.0f;      // keeps the HUD up briefly after the meter fills
    float m_HudAlpha = 0.0f;         // HUD fade 0..1
    bool m_InPlay = false;
    float m_LastHudDt = 0.016f;
    std::chrono::steady_clock::time_point m_LastFrame = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point m_HudLast = std::chrono::steady_clock::now();
    std::atomic<uint32_t> m_FrameCount{0};
    std::atomic<int64_t> m_LastUpdateTicks{0}; // last time the game loop ran (lets the HUD hide in menus)

    // --- Settings (persisted) ---
    int m_InstinctKey = 0x10;        // VK_SHIFT: set this to your in-game Instinct key
    bool m_ToggleMode = false;       // false = hold, true = press to toggle
    float m_FocusScale = 0.25f;      // time scale while Instinct is active
    float m_BlendSpeed = 6.0f;       // higher = snappier transition
    float m_DrainSeconds = 15.0f;    // real seconds from full to empty
    float m_RechargeSeconds = 30.0f; // real seconds from empty to full
    float m_RechargeDelay = 1.0f;    // pause before recharging after use
    bool m_BlockEnabled = true;      // block the Instinct key while Instinct is unusable

    bool m_HudEnabled = true;
    bool m_HudAlways = false;        // keep the meter visible even when full
    float m_HudWidth = 0.17f;        // fraction of screen width
    float m_HudYOffset = 64.0f;      // pixels from the bottom (at 1080p)
    float m_HudScale = 1.0f;
    float m_HudOpacity = 0.95f;
    float m_FillColor[3] = {0.93f, 0.93f, 0.90f};
    float m_LowColor[3] = {0.86f, 0.20f, 0.18f};

    bool m_SoundEnabled = true;
    int m_SoundSource = 0;           // 0 = preset / folder, 1 = custom file
    float m_SoundVolume = 0.6f;
    bool m_FadeOutOn = true;
    float m_FadeOutMs = 400.0f;
    bool m_FadeInOn = true;
    float m_FadeInMs = 300.0f;
    std::string m_PresetName;        // selected file in the sound folder (stored in a small text file)
    char m_CustomPath[520] = {};     // custom file path (stored in the same text file)

    // --- Sound state ---
    std::wstring m_ModDir;
    std::wstring m_SoundDir;
    std::vector<std::string> m_PresetFiles;
    int m_PresetIndex = -1;
    std::string m_SoundStatus;
    bool m_SndOpen = false;
    bool m_SndPlaying = false;
    float m_SndGain = 0.0f;          // fade factor 0..1
    int m_SndDir = 0;                // +1 fading in, -1 fading out, 0 steady
    bool m_Testing = false;
    bool m_SoundWasWanted = false;

    // --- UI state ---
    bool m_ShowUI = false;
    bool m_Binding = false;
    bool m_BindArmed = false;
};

DECLARE_ZHM_PLUGIN(FocusForAll)
