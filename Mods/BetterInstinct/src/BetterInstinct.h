#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "IPluginInterface.h"
#include "Hooks.h"

#include <Glacier/ZActor.h>
#include <Glacier/ZCameraEntity.h>
#include <Glacier/ZCollision.h>
#include <Glacier/ZEntity.h>
#include <Glacier/ZHitman5.h>
#include <Glacier/ZInput.h>
#include <Glacier/ZObject.h>

// Better Instinct: Instinct slows time and drains a meter. When the meter is too low Instinct is
// blocked entirely, and the looping Instinct sound only plays while Instinct is really usable.
class BetterInstinct : public IPluginInterface {
public:
    ~BetterInstinct() override;

    void Init() override;
    void OnEngineInitialized() override;
    void OnDrawMenu() override;
    void OnDrawUI(bool p_HasFocus) override;

private:
    void OnFrameUpdate(const SGameUpdateEvent& p_UpdateEvent);

    void LoadSettings();
    void SaveSettings();

    // Own settings file (%APPDATA%\BetterInstinct\BetterInstinct.cfg): written on every change, so options never get lost.
    bool SaveConfigFile();
    bool LoadConfigFile();
    bool m_ConfigReady = false;
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

    // --- Point shooting (Absolution / Dead Eye style) ---
    // Hold the PS key: time nearly stops, aim at enemies and tag them (click or hover). Release: Agent 47 snaps from
    // target to target and every tagged enemy is shot, with muzzle flash, kill confirmation and sound.
    void UpdatePointShooting(float p_Dt, bool p_GameFocused);
    void ResetPointShooting();
    void DrawPsOverlay();
    bool DrawKeyBinder(const char* p_Id, int& p_Key, bool& p_Binding, bool& p_Armed);
    bool PsAimPoint(const TEntityRef<ZActor>& p_Actor, SVector3& p_Out);
    bool PsLineOfSight(const float* p_Cam, const SVector3& p_To, ZActor* p_Actor);
    void PsPlaySound(const wchar_t* p_File);

    // Game-level input blocking: the game asks ZInputAction_Digital "is action X pressed?" for every
    // action each frame. While Instinct is unusable we answer "no" for the Instinct action(s).
    void RebuildPatterns();
    bool ActionMatches(const char* p_Name);

    DECLARE_PLUGIN_DETOUR(BetterInstinct, bool, ZInputAction_Digital, ZInputAction* th, int32_t controllerId);

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
    std::atomic<bool> m_HasPlayer{false};      // Agent 47 exists (false in the main menu / hub screens)
    float m_NoPlayerTime = 0.0f;               // how long there has been no player (resets the meter after a moment)

    // Sparkle particles shown at the tip of the meter while Instinct is in use.
    struct Particle {
        float x, y, vx, vy, life, maxLife, size;
    };
    std::vector<Particle> m_Particles;
    float m_EmitAcc = 0.0f;
    uint32_t m_Rng = 0x1234567u;
    float Rand01();

    // --- Point shooting state ---
    enum PsStateId : int { PS_IDLE = 0, PS_AIMING = 1, PS_EXECUTING = 2 };
    struct PsTarget {
        TEntityRef<ZActor> m_Ref;
        SVector3 m_Aim;          // world position of the aim point (head / torso), refreshed every game update
        float m_Dist = 0.0f;     // distance to the camera when tagged
        float m_Age = 0.0f;      // real seconds since tagged (marker pop-in)
    };
    struct PsFlash {
        SVector3 m_Pos;
        float m_Age = 0.0f;
        bool m_Kill = false;     // true = shot/kill confirmation, false = tag ping
    };
    std::atomic<int> m_PsState{PS_IDLE};
    std::atomic<int> m_PsTagCount{0};          // tagged targets still waiting to be shot (HUD pips)
    std::atomic<float> m_PsPunch{0.0f};        // screen flash when a shot goes off
    std::atomic<float> m_ScreenW{1920.0f};     // cached by the UI thread, used by the game thread for targeting
    std::atomic<float> m_ScreenH{1080.0f};
    std::mutex m_PsMutex;                      // guards everything below up to m_PsPrevDown
    std::vector<PsTarget> m_PsTargets;
    std::vector<PsFlash> m_PsFlashes;
    ZActor* m_PsHoverActor = nullptr;
    bool m_PsHoverValid = false;
    bool m_PsHoverCan = false;
    SVector3 m_PsHoverAim;
    float m_PsHoverFrac = 0.0f;                // hover dwell progress 0..1
    SVector3 m_PsLastAim;
    bool m_PsLastAimValid = false;
    int m_PsExecPhase = 0;                     // 0 = snapping to the next target, 1 = pause after a shot
    float m_PsExecTimer = 0.0f;
    bool m_PsPrevDown = false;
    uint32_t m_PsLastClicks = 0;
    float m_PsFade = 0.0f;                     // UI thread only
    float m_PsReticleX = 0.0f;
    float m_PsReticleY = 0.0f;
    bool m_PsReticleSet = false;

    // --- Game-level input block state ---
    std::atomic<bool> m_InputBlock{false};     // detour answers "not pressed" for matching actions
    std::atomic<bool> m_CaptureNames{false};   // detour records action names (Debug section open)
    std::atomic<uint32_t> m_BlockedPolls{0};   // how many times the detour actually blocked something
    bool m_ForceBlockTest = false;             // debug: block regardless of the meter
    char m_ActionPatterns[256] = "instinct,focus"; // comma-separated, case-insensitive parts of action names
    char m_NameFilter[64] = {};
    std::mutex m_NameMutex;
    std::vector<std::string> m_Patterns;       // lowercase, parsed from m_ActionPatterns
    std::set<std::string> m_SeenNames;         // every action name the game has polled (debug)

    // --- Settings (persisted) ---
    int m_InstinctKey = 0x11;        // VK_CONTROL (either Ctrl): set this to your in-game Instinct key
    bool m_ToggleMode = false;       // false = hold, true = press to toggle
    float m_FocusScale = 0.50f;      // time scale while Instinct is active
    float m_BlendSpeed = 6.0f;       // higher = snappier transition
    float m_DrainSeconds = 15.0f;    // real seconds from full to empty
    float m_RechargeSeconds = 30.0f; // real seconds from empty to full
    float m_RechargeDelay = 1.0f;    // pause before recharging after use
    bool m_BlockEnabled = true;      // block the Instinct key while Instinct is unusable

    bool m_HipAssist = true;         // during Instinct, hold left clicks until they lasted long enough in game time (hip fire)
    int m_HipGameMs = 200;           // minimum click length in GAME milliseconds (real time = this / time scale)

    bool m_PsEnabled = true;         // point shooting on/off
    int m_PsKey = 0x05;              // VK_XBUTTON1 (mouse 4): hold to point shoot
    int m_PsTagMode = 0;             // 0 = click to tag, 1 = hover to tag
    float m_PsHoverDwell = 0.35f;    // real seconds the crosshair has to rest on an enemy (hover mode)
    float m_PsScale = 0.12f;         // time scale while aiming
    float m_PsExecScale = 0.30f;     // time scale while the shots play out
    float m_PsTagCost = 0.10f;       // meter used up per tagged target (0..1)
    int m_PsMaxTags = 6;             // most targets that can be tagged at once
    float m_PsDrainMul = 0.5f;       // meter drain while aiming, compared to normal Instinct
    float m_PsSnapRadius = 0.09f;    // how close to the crosshair an enemy has to be, as a fraction of screen height
    float m_PsMaxDist = 70.0f;       // farthest taggable enemy (meters)
    int m_PsAimPart = 0;             // 0 = head, 1 = torso
    float m_PsSnapTime = 0.12f;      // real seconds the aim takes to snap to the next target
    float m_PsShotDelay = 0.15f;     // real seconds between a shot and the next snap
    int m_PsOrder = 0;               // 0 = order of tagging, 1 = nearest first
    bool m_PsBlockFire = true;       // hover mode: also swallow left click while point shooting (click mode always does)
    bool m_PsSoundOn = true;         // play PointTag.wav / PointShot.wav from the InstinctSounds folder if present
    float m_PsHeadHeight = 1.62f;    // aim point height above the actor origin (meters)
    float m_PsTorsoHeight = 1.25f;
    bool m_PsYUp = false;            // debug: world up axis is Y instead of Z

    bool m_HudEnabled = true;
    bool m_HudAlways = false;        // keep the meter visible even when full
    float m_HudWidth = 0.17f;        // fraction of screen width
    float m_HudYOffset = 64.0f;      // pixels from the bottom (at 1080p)
    float m_HudScale = 1.0f;
    float m_HudOpacity = 0.95f;
    float m_FillColor[3] = {1.00f, 0.78f, 0.28f}; // gold
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
    bool m_BindingPs = false;
    bool m_BindArmedPs = false;
};

DECLARE_ZHM_PLUGIN(BetterInstinct)
