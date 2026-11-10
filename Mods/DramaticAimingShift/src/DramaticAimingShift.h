#pragma once

#include <chrono>
#include <string>

#include <IPluginInterface.h>

#include <Glacier/ZEntity.h>
#include <Glacier/ZObject.h>

// Port of "Dramatic Aiming Mod" v1.2.1 (Dreant) to a ZHMModSDK plugin.
// Slow-mo + grey overlay + Ave Maria while aiming a gun, but ONLY while Shift is held.
class DramaticAimingShift : public IPluginInterface {
public:
    DramaticAimingShift();
    ~DramaticAimingShift() override;

    void Init() override;
    void OnEngineInitialized() override;
    void OnDrawMenu() override;
    void OnDrawUI(bool p_HasFocus) override;

private:
    // Per-frame logic (registered with the game loop).
    void OnFrameUpdate(const SGameUpdateEvent& p_UpdateEvent);

    // Mirrors the original mod, which listened to the EnterAimAt / ExitAimAt
    // output pins of the game's ZLookAtAimSoundController.
    DECLARE_PLUGIN_DETOUR(DramaticAimingShift, bool, OnPinOutput, ZEntityRef p_Entity, uint32_t p_PinId, const ZObjectRef& p_Data);

    bool IsShiftHeld() const;
    void BeginEffect();
    void EndEffect();

    // --- game-facing shims (the only places that touch engine internals) ---
    void SetTimeScale(float p_Scale);

    // --- audio (MCI, files live next to the plugin DLL) ---
    void PlayMusic();
    void StopMusic();
    void PlayOneShot(const wchar_t* p_Alias, const std::wstring& p_File);
    void UpdateMusicFade(float p_Dt);
    std::wstring FindAudio(const wchar_t* p_BaseName) const;

private:
    // ---- settings (values taken from the original mod) ----
    bool  m_Enabled         = true;
    bool  m_RequireShift    = true;   // the new requirement
    float m_TimeScale       = 0.3f;   // "Poll aiming" -> SetValue 0.3
    float m_SlowMoDelay     = 0.150f; // "Wait for finish aiming animation" 150 ms
    float m_OverlayDelay    = 0.065f; // "..._Overlay" timer 65 ms
    float m_OverlayFade     = 0.200f; // PauseBlendController_2 blend in/out 0.2 s
    float m_OverlayAlphaMax = 0.45f;  // tweakable: strength of the grey wash
    float m_MusicFade       = 0.100f; // AudioEvent_AveMaria m_fTransitionTime
    float m_MusicVolume     = 0.8f;
    bool  m_PlayMusic       = true;

    // ---- runtime state ----
    bool  m_AimPin       = false;  // between EnterAimAt and ExitAimAt
    bool  m_Active       = false;  // effect currently running
    bool  m_TimeApplied  = false;
    float m_ActiveTime   = 0.f;
    float m_OverlayAlpha = 0.f;    // 0..1
    float m_MusicGain    = 0.f;    // 0..1 (fade)
    bool  m_MusicPlaying = false;
    bool  m_MusicStopping= false;

    uint32_t m_PinEnterAim = 0;
    uint32_t m_PinExitAim  = 0;

    std::chrono::steady_clock::time_point m_LastTick{};
};

DECLARE_ZHM_PLUGIN(DramaticAimingShift)
