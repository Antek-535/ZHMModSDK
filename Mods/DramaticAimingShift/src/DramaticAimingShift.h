#pragma once

#include <atomic>
#include <chrono>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <IPluginInterface.h>

#include <Glacier/ZEntity.h>
#include <Glacier/ZObject.h>
#include <Glacier/ZString.h>

struct SLutGpu; // GPU-side LUT state (defined in the .cpp)

// Port of "Dramatic Aiming Mod" v1.2.1 (Dreant) to a ZHMModSDK plugin.
// Slow-mo + grey colour-grading overlay + custom sound while aiming a gun AND holding Shift.
// The overlay uses the same technique as BetterInstinct: it rewrites the game's own colour-grading LUT,
// so it is a real scene overlay and never touches the HUD.
class DramaticAimingShift : public IPluginInterface {
public:
    DramaticAimingShift();
    ~DramaticAimingShift() override;

    void Init() override;
    void OnEngineInitialized() override;
    void OnDrawMenu() override;

private:
    void OnFrameUpdate(const SGameUpdateEvent& p_UpdateEvent);

    // Same signals the original mod listened to (ZLookAtAimSoundController).
    DECLARE_PLUGIN_DETOUR(DramaticAimingShift, bool, OnPinOutput, ZEntityRef p_Entity, uint32_t p_PinId, const ZObjectRef& p_Data);

    // Probe: does the game ask for its Instinct outline setting through the application-option hook?
    DECLARE_PLUGIN_DETOUR(DramaticAimingShift, bool, GetOption, const ZString& p_OptionName, bool p_Default);

    bool IsShiftHeld() const;
    void BeginEffect();
    void EndEffect();
    void SetTimeScale(float p_Scale);

    // --- settings ---
    void LoadSettings();
    void SaveSettings();

    // --- sound (MCI: wav / mp3, looping, fades) ---
    void ScanSounds();
    std::wstring CurrentSoundPath() const;
    void StartSound();
    void StopSound();
    void UpdateSound(float p_Dt);
    void ApplySoundVolume();
    void CloseSound();

    // --- overlay (game colour-grading LUT, same method as BetterInstinct) ---
    void SetLutStatus(int p_State, const std::string& p_Text);
    void ReleaseLut();
    bool LutStillValid() const;
    bool EnsureLut();
    void RestoreTint();
    void UpdateTint(float p_Dt, bool p_Active);

private:
    // ---- settings ----
    bool  m_Enabled         = true;
    bool  m_RequireShift    = true;
    float m_TimeScale       = 0.3f;   // original: 0.3
    float m_SlowMoDelay     = 0.150f; // original: 150 ms
    float m_OverlayDelay    = 0.065f; // original: 65 ms

    bool  m_SoundEnabled    = true;
    float m_SoundVolume     = 0.8f;
    float m_SoundFade       = 0.100f; // original: 0.1 s transition

    bool  m_UseInstinct     = true;   // overlay is only drawn by the game while its Instinct state is on
    int   m_InstinctKeyIdx  = 0;
    bool  m_InstinctToggle  = false;  // Instinct is set to Toggle (not Hold) in the game settings

    bool  m_TintEnabled     = true;
    float m_InstinctTint[3] = {1.0f, 1.0f, 1.0f}; // white = pure grey
    float m_TintStrength    = 0.5f;               // original grey overlay: 0.5
    bool  m_TintKeepBrightness = false;
    float m_TintBrightness  = 1.0f;
    int   m_LutBarrier      = 0;

    // ---- runtime state ----
    std::atomic<bool> m_AimPin{false};
    std::atomic<bool> m_ExitIgnored{false};
    std::atomic<bool> m_SuppressOutlines{false}; // true while the effect is active
    std::mutex m_OptMutex;
    std::set<std::string> m_SeenOptions;
    bool  m_Active       = false;
    bool  m_TimeApplied  = false;
    float m_ActiveTime   = 0.f;
    bool  m_InstinctHeld = false; // we are holding the injected Instinct key
    bool  m_InstinctToggledOn = false; // toggle mode: we switched Instinct on and must switch it off
    bool  m_TapPending = false;        // a key tap is waiting for its key-up
    int   m_TapVk = 0;
    std::chrono::steady_clock::time_point m_TapReleaseAt{};

    uint32_t m_PinEnterAim = 0;
    uint32_t m_PinExitAim  = 0;
    std::chrono::steady_clock::time_point m_LastTick{};

    // sound
    std::wstring m_SoundDir;
    std::vector<std::string> m_SoundFiles;
    int   m_SoundIndex   = 0;
    bool  m_SndOpen      = false;
    bool  m_SndWanted    = false;
    float m_SndGain      = 0.f;
    std::string m_SoundStatus;

    // overlay / LUT
    std::atomic<bool> m_HasPlayer{false};
    float m_NoPlayerTime = 0.f;
    SLutGpu* m_Lut = nullptr;
    bool  m_LutApplied = false;
    uint32_t m_LutAppliedSig = 0;
    std::atomic<uint32_t> m_LutFailSig{0};
    std::atomic<bool> m_LutReread{false};
    float m_LutPoll = 0.f;
    float m_LutCooldown = 0.f;
    int   m_LutState = 0;
    int   m_LutWrites = 0;
    std::mutex m_LutMutex;
    std::string m_LutStatus;
};

DECLARE_ZHM_PLUGIN(DramaticAimingShift)
