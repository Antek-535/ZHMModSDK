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
#include <Glacier/ZItem.h>
#include <Glacier/ZModule.h>
#include <Glacier/ZScene.h>

// Better Instinct: Instinct slows time and drains a meter. When the meter is too low Instinct is
// blocked entirely, and the looping Instinct sound only plays while Instinct is really usable.
constexpr int kGlowCatCount = 11; // number of Instinct glow categories (Targets, Civilians, Guards, ...)
struct SLutGpu; // GPU-side state of the Instinct color-grading texture (defined in the cpp)

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

    // Custom Instinct color. The grey Instinct look is a color-grading lookup table (LUT) texture of the scene render.
    // We recolor a copy of the game's own LUT and write it into that texture, so the grey is replaced, not mixed, and
    // the UI, menus and map are never affected (it is not an overlay).
    void UpdateTint(float p_Dt, bool p_Active);
    void RestoreTint();
    bool EnsureLut();
    bool LutStillValid() const;
    void ReleaseLut();
    void SetLutStatus(int p_State, const std::string& p_Text);
    SLutGpu* m_Lut = nullptr;
    bool m_LutApplied = false;                     // our recolored LUT is currently in the texture
    uint32_t m_LutAppliedSig = 0;                  // settings the texture currently reflects
    std::atomic<uint32_t> m_LutFailSig{0};         // settings that failed to write (not retried until they change)
    float m_LutPoll = 0.0f;
    float m_LutCooldown = 0.0f;
    std::atomic<bool> m_LutReread{false};
    std::atomic<int> m_LutState{0};                // 0 idle, 1 not found yet, 2 error, 3 ready, 4 active
    std::atomic<uint32_t> m_LutWrites{0};
    std::string m_LutStatus;                       // guarded by m_LutMutex
    std::mutex m_LutMutex;

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

    // Instinct glow colors (people and items). The game keeps them as properties of a config entity
    // (m_GlowCivilianColor, m_GlowEnemiesOutlineColor, ...). We find that entity at runtime (by those property names)
    // and write the colors into it. "Color" is the silhouette (fill), "OutlineColor" the outline.
    struct SGlowSetting {
        int Mode = 0;                 // 0 = game default, 1 = custom
        bool FillOn = true;           // custom mode: show the silhouette
        bool OutlineOn = true;        // custom mode: show the outline
        float Fill[4] = {};           // RGBA 0..1
        float Outline[4] = {};
        float VanFill[4] = {};        // the game's own values (read from the game, not saved)
        float VanOutline[4] = {};
        bool HasVanilla = false;
        bool Seeded = false;          // custom colors have been set (by the user, or copied from the game's colors)
    };
    struct SGlowSlot {
        uint8_t* Ptr = nullptr;       // address of the color inside the game's entity
        uint32_t Size = 16;           // 16 = RGBA floats, 12 = RGB floats
        ZEntityRef Ref;               // the entity the color belongs to
        uint32_t PropId = 0;          // property id (so the game's change handlers can be told about the new color)
        int Cat = 0;
        bool Outline = false;
        float Vanilla[4] = {};
    };
    void InitGlowDefaults();
    void ScanGlowSlots(bool p_KeepSnapshots);
    void UpdateGlowColors(float p_Dt);
    void WriteGlowSlots(bool p_NotifyGame);
    void RestoreGlowColors();
    uint32_t GlowSignature() const;
    SGlowSetting m_Glow[kGlowCatCount];
    std::vector<SGlowSlot> m_GlowSlots;           // game thread only
    bool m_GlowNeedScan = true;
    bool m_GlowKeepSnap = false;
    bool m_GlowForce = true;
    float m_GlowWait = 3.0f;
    float m_GlowTimer = 0.0f;
    uint32_t m_GlowAppliedSig = 0;
    std::atomic<bool> m_GlowRescanRequest{false};
    std::atomic<bool> m_GlowDirty{false};         // the game thread changed a setting: save it
    std::atomic<int> m_GlowState{0};              // 0 not scanned, 1 not found, 2 found
    std::atomic<uint32_t> m_GlowFound{0};
    std::atomic<uint32_t> m_GlowEntities{0};
    std::atomic<uint32_t> m_GlowSkipped{0};
    std::atomic<uint32_t> m_GlowScanned{0};

    bool InstinctKeyDown() const;
    void ForceReleaseInstinct();

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

    bool m_HudEnabled = true;
    bool m_HudAlways = false;        // keep the meter visible even when full
    float m_HudWidth = 0.17f;        // fraction of screen width
    float m_HudYOffset = 64.0f;      // pixels from the bottom (at 1080p)
    float m_HudScale = 1.0f;
    float m_HudOpacity = 0.95f;
    float m_FillColor[3] = {1.00f, 0.78f, 0.28f}; // gold
    float m_LowColor[3] = {0.86f, 0.20f, 0.18f};

    bool m_TintRemove = false;       // no Instinct color at all: the grey grading is replaced by an identity LUT (outlines only)
    bool m_TintEnabled = false;      // off by default, so existing setups look exactly as before
    float m_InstinctTint[3] = {0.30f, 0.70f, 1.00f}; // the color that replaces Instinct's grey
    float m_TintStrength = 1.00f;    // 1 = the grey is completely replaced, lower keeps part of the vanilla look
    float m_TintBrightness = 1.00f;  // brightness of the replacement color
    bool m_TintKeepBrightness = true; // keep the picture about as bright as vanilla Instinct (dark colors would be much darker)
    int m_LutBarrier = 0;            // GPU state the texture is in while the game isn't using it: 0 = shader resource, 1 = common

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

DECLARE_ZHM_PLUGIN(BetterInstinct)
