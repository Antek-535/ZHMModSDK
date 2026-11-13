#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
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

    // Better Zoom: hold right mouse while the hands are empty (or hold something that can't be thrown) and turn the mouse
    // wheel to zoom in further than the game allows. The game's over-the-shoulder camera profiles have a field of view
    // (m_fFOV); we remember their vanilla values and write a narrower one while zooming.
    void UpdateZoom(float p_Dt);
    bool HeldItemAllowsZoom();
    void WriteZoomFov(float p_Zoom);
    void DumpHeldItemInfo(const wchar_t* p_FileName);                       // diagnostic: writes the player's character/inventory properties to BetterInstinct.log
    std::atomic<int> m_DumpRequest{0};             // 1 = dump with empty hands, 2 = dump while holding a weapon
    void CheckTemplateForZoom(ZTemplateEntityFactory* p_Factory, ZEntityType** p_Entity);
    struct SZoomProfile {
        ZEntityRef Ref;
        float VanillaFov = 30.0f;
        const float* Ptr = nullptr;                // where the profile's m_fFOV lives (read back for the UI)
    };
    // Live camera: float properties with "fov" in their name on the player's main camera entity.
    struct SCamFov {
        float* Ptr = nullptr;                      // direct memory (null when the property only has a getter/setter)
        ZEntityRef Ref;                            // owner entity (used for getter/setter properties)
        uint32_t PropId = 0;
        float Base = 0.0f;                         // the game's own value
        float Last = 0.0f;                         // what we wrote last (0 = we are not overriding)
        std::string Name;
    };
    bool ReadCamFov(const SCamFov& p_F, float& p_Out);
    void WriteCamFov(const SCamFov& p_F, float p_Value);
    std::vector<SCamFov> m_CamFovs;                // game thread only
    float m_CamScan = 0.0f;
    bool m_PrevRightDown = false;

    // Held-item check: the zoom only works with empty hands. The game's character/inventory objects are searched for
    // reflected properties that say "something is in the hands" (held/current/equipped/carried item, weapon in hand...).
    // A property only counts after it has been seen empty once, so constant pointers can never block the zoom for good.
    struct SItemProbe {
        ZEntityRef Ref;
        const uint8_t* Ptr = nullptr;              // where the value lives inside the game's entity
        uint32_t Size = 0;                         // 1 = flag, 8 / 16 = entity reference
        std::string Name;
        bool SeenEmpty = false;                    // has been empty at least once (so it is a real indicator)
    };
    std::vector<SItemProbe> m_ItemProbes;          // game thread only
    float m_ItemScan = 0.0f;
    bool m_ItemHolding = false;                    // something is in 47's hands right now
    std::atomic<int> m_ItemState{0};               // 0 no property found, 1 learning, 2 hands empty, 3 holding something (UI)
    std::string m_ItemInfo;                        // names of the properties used (UI), guarded by m_ZoomMutex
    void ScanItemProbes();
    bool ReadItemProbe(const SItemProbe& p_P, bool& p_Out);
    void UpdateHeldItem(float p_Dt);
    void ScanCameraFov();
    void ApplyCameraFov(float p_Zoom);
    std::atomic<float> m_ZoomReadback{0.0f};       // profile FOV as read back from memory
    std::atomic<float> m_ZoomReadbackVan{0.0f};
    std::string m_CamInfo;                         // guarded by m_ZoomMutex
    std::string m_CamLive;                         // live values of the camera FOV properties (UI), guarded by m_ZoomMutex
    float m_CamLiveTimer = 0.0f;
    std::atomic<bool> m_ZoomGateNow{false};        // zoom is allowed right now (UI)
    // Focus diagnostics: memory of the camera / player objects before and shortly after right mouse goes down; the values
    // that changed show where the game keeps the "focus" zoom (logged to BetterInstinct_zoom.log).
    struct SMemSnap {
        std::string Label;
        uintptr_t Base = 0;
        std::vector<uint8_t> Data;
    };
    std::vector<SMemSnap> TakeSnaps();
    std::vector<SMemSnap> m_SnapA;
    float m_DiagTimer = -1.0f;
    int m_DiagCount = 0;
    std::string m_ZoomLogBuf;                      // zoom diagnostics, written to BetterInstinct_zoom.log
    float m_ZoomLogTimer = 0.0f;
    bool m_ZoomLogDirty = false;
    std::atomic<int> m_WheelSeen{0};               // total wheel delta the zoom has received (UI)
    std::vector<SZoomProfile> m_ZoomProfiles;      // guarded by m_ZoomMutex
    std::mutex m_ZoomMutex;
    float m_ZoomTarget = 1.0f;                     // what the wheel asked for
    float m_ZoomCur = 1.0f;                        // smoothed value actually applied
    float m_ZoomWritten = 1.0f;                    // last value written to the profiles
    std::atomic<uint32_t> m_ZoomFound{0};          // camera profiles found (shown in the UI)
    std::atomic<uint32_t> m_ZoomWrites{0};

    bool InstinctKeyDown() const;
    void ForceReleaseInstinct();

    // Game-level input blocking: the game asks ZInputAction_Digital "is action X pressed?" for every
    // action each frame. While Instinct is unusable we answer "no" for the Instinct action(s).
    void RebuildPatterns();
    bool ActionMatches(const char* p_Name);

    DECLARE_PLUGIN_DETOUR(BetterInstinct, bool, ZInputAction_Digital, ZInputAction* th, int32_t controllerId);

    // Entity activation hook: every entity the game activates passes through here, so the Instinct glow config
    // entity (wherever it is nested in the scene) is caught the moment it is activated.
    DECLARE_PLUGIN_DETOUR(BetterInstinct, void, ZEntityManager_ActivateEntity, ZEntityManager* th, ZEntityRef* entity, void* a3);
    void CatchGlowEntity(const ZEntityRef* p_Ref);
    // Writes the chosen colors straight into the glow config entity while the game is activating it, i.e. BEFORE the
    // game reads them (the same moment a data patch would change them).
    void ApplyGlowOnActivate(const ZEntityRef* p_Ref);
    // Template hook: runs right after the game has built an entity template and set all its properties from the data.
    // The Instinct glow config entity is found here by its entity id, and its colors are written before anything reads them.
    DECLARE_PLUGIN_DETOUR(BetterInstinct, void, ZTemplateEntityFactory_ConfigureEntity, ZTemplateEntityFactory* th, ZEntityType** pEntity, const SExternalReferences& externalRefs, uint8_t* unk0);
    void CheckTemplateForGlow(ZTemplateEntityFactory* p_Factory, ZEntityType** p_Entity);
    std::atomic<uint32_t> m_GlowTplCalls{0};                // how often the template hook ran (shows the hook is alive)
    std::atomic<uint32_t> m_GlowTplHits{0};                 // how often it found an entity with the glow config id
    std::mutex m_PreMutex;
    std::map<uintptr_t, std::array<float, 4>> m_PreVanilla; // color address -> the game's own value, remembered before we write
    std::atomic<uint32_t> m_GlowPreApplied{0};              // how many color values were written at activation (shown in the UI)
    std::mutex m_CatchMutex;
    std::vector<ZEntityRef> m_GlowCaught;           // entities that have the glow color properties
    std::map<ZEntityType*, bool> m_CatchTypeCache;  // entity type -> has glow color properties
    std::vector<uint32_t> m_CatchAnchors;           // property ids that identify the glow config entity

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
        float phase = 0.0f;   // random offset for the flame flicker
        bool ember = false;   // small fast spark instead of a flame
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

    bool m_ZoomEnabled = true;       // wheel zoom while holding right mouse (empty hands / non-throwable item)
    float m_ZoomMax = 5.0f;          // furthest zoom, 1 = vanilla
    float m_ZoomStep = 1.15f;        // zoom multiplier per wheel notch
    float m_ZoomSmooth = 10.0f;      // higher = snappier
    bool m_ZoomRemember = false;     // keep the zoom level for the next time you aim (default: back to 1x on release)
    bool m_ZoomLiveCam = true;        // true = scale the live camera FOV (works); false = scale the UnAimed camera profiles (never found them in the logs)
    bool m_ZoomAnyItem = false;      // skip the held-item check (zoom with anything in hand): off, zoom is for empty hands only

    bool m_HudEnabled = true;
    bool m_HudAlways = false;        // keep the meter visible even when full
    bool m_HudVertical = false;      // false = horizontal bar, true = vertical bar (fills upward)
    float m_HudX = 0.5f;             // horizontal position of the bar's center: 0 = left edge, 1 = right edge
    bool m_FireFx = true;            // flame-style particles at the tip of the meter (Absolution look)
    float m_FireAmount = 1.0f;       // how many flame particles
    float m_HudWidth = 0.17f;        // fraction of screen width
    float m_HudYOffset = 64.0f;      // pixels from the bottom (at 1080p)
    float m_HudScale = 1.0f;
    float m_HudOpacity = 0.95f;
    float m_FillColor[3] = {1.00f, 0.78f, 0.28f}; // gold
    float m_LowColor[3] = {0.86f, 0.20f, 0.18f};
    float m_BarBottom[3] = {1.00f, 0.50f, 0.10f}; // color at the bottom (start) of the bar: orange
    float m_BarTop[3] = {1.00f, 0.88f, 0.25f};    // color at the top (end) of the bar: yellow
    bool m_ShowIcon = true;                       // round flame icon at the bottom of the bar

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
