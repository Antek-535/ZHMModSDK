#include "DramaticAimingShift.h"

#include <Hooks.h>
#include <Logging.h>
#include <Globals.h>
#include <Functions.h>

#include <Glacier/ZGameLoopManager.h>
#include <Glacier/ZGameTime.h>

#include <imgui.h>

#include <algorithm>
#include <cstdint>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

namespace {
    // Glacier pin / property ids are the CRC32 of their name.
    uint32_t Crc32(const char* p_Str) {
        uint32_t s_Crc = 0xFFFFFFFFu;
        for (; *p_Str; ++p_Str) {
            s_Crc ^= static_cast<uint8_t>(*p_Str);
            for (int i = 0; i < 8; ++i)
                s_Crc = (s_Crc >> 1) ^ (0xEDB88320u & (0u - (s_Crc & 1u)));
        }
        return ~s_Crc;
    }

    std::wstring ModuleDir() {
        HMODULE s_Module = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&ModuleDir), &s_Module);
        wchar_t s_Path[MAX_PATH]{};
        GetModuleFileNameW(s_Module, s_Path, MAX_PATH);
        std::wstring s_Dir = s_Path;
        return s_Dir.substr(0, s_Dir.find_last_of(L"\\/") + 1);
    }

    bool GameWindowFocused() {
        DWORD s_Pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &s_Pid);
        return s_Pid == GetCurrentProcessId();
    }

    void Mci(const std::wstring& p_Cmd) {
        mciSendStringW(p_Cmd.c_str(), nullptr, 0, nullptr);
    }
}

DramaticAimingShift::DramaticAimingShift() {
    m_PinEnterAim = Crc32("EnterAimAt");
    m_PinExitAim  = Crc32("ExitAimAt");
}

DramaticAimingShift::~DramaticAimingShift() {
    EndEffect();
    Mci(L"close DAMusic");
    Hooks::SignalOutputPin->RemoveDetoursWithContext(this);

    const ZMemberDelegate<DramaticAimingShift, void(const SGameUpdateEvent&)> s_Delegate(this, &DramaticAimingShift::OnFrameUpdate);
    Globals::GameLoopManager->UnregisterFrameUpdate(s_Delegate, 1, EUpdateMode::eUpdatePlayMode);
}

void DramaticAimingShift::Init() {
    Hooks::SignalOutputPin->AddDetour(this, &DramaticAimingShift::OnPinOutput);
}

void DramaticAimingShift::OnEngineInitialized() {
    const ZMemberDelegate<DramaticAimingShift, void(const SGameUpdateEvent&)> s_Delegate(this, &DramaticAimingShift::OnFrameUpdate);
    Globals::GameLoopManager->RegisterFrameUpdate(s_Delegate, 1, EUpdateMode::eUpdatePlayMode);
    m_LastTick = std::chrono::steady_clock::now();
}

// ---------------------------------------------------------------------------
// Aim detection: same signal the original mod used.
// ---------------------------------------------------------------------------
DEFINE_PLUGIN_DETOUR(DramaticAimingShift, bool, OnPinOutput, ZEntityRef p_Entity, uint32_t p_PinId, const ZObjectRef& p_Data) {
    if (p_PinId == m_PinEnterAim) {
        m_AimPin = true;
        m_ExitIgnored = false;
    } else if (p_PinId == m_PinExitAim) {
        // Pressing Shift (a movement key) while aiming makes the game drop its aim state.
        // If Shift and right-click are both held, treat that as still aiming.
        if (m_RequireShift && IsShiftHeld() && (GetAsyncKeyState(VK_RBUTTON) & 0x8000)) {
            m_ExitIgnored = true;
        } else {
            m_AimPin = false;
            m_ExitIgnored = false;
        }
    }

    return HookResult<bool>(HookAction::Continue());
}

bool DramaticAimingShift::IsShiftHeld() const {
    return GameWindowFocused() && (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
}

// ---------------------------------------------------------------------------
// Per-frame state machine (timings from the original entity graph)
// ---------------------------------------------------------------------------
void DramaticAimingShift::OnFrameUpdate(const SGameUpdateEvent&) {
    // Real (unscaled) delta so slow-mo doesn't slow our own timers/fades.
    const auto s_Now = std::chrono::steady_clock::now();
    const float s_Dt = std::min(0.1f, std::chrono::duration<float>(s_Now - m_LastTick).count());
    m_LastTick = s_Now;

    // Safety: if we ignored an exit, end the aim once right-click is let go.
    if (m_ExitIgnored && !(GetAsyncKeyState(VK_RBUTTON) & 0x8000)) {
        m_AimPin = false;
        m_ExitIgnored = false;
    }

    const bool s_Want = m_Enabled && m_AimPin && (!m_RequireShift || IsShiftHeld());

    if (s_Want && !m_Active)
        BeginEffect();
    else if (!s_Want && m_Active)
        EndEffect();

    if (m_Active) {
        m_ActiveTime += s_Dt;

        // "Wait for finish aiming animation" (150 ms) -> game time multiplier 0.3
        if (!m_TimeApplied && m_ActiveTime >= m_SlowMoDelay)
            m_TimeApplied = true;

        // The game can reset the multiplier on its own (e.g. when movement state changes),
        // so keep re-applying it every frame while active.
        if (m_TimeApplied)
            SetTimeScale(m_TimeScale);
    }

    // Grey overlay: starts after 65 ms, blends in/out over 0.2 s.
    const bool  s_OverlayOn = m_Active && m_ActiveTime >= m_OverlayDelay;
    const float s_Step      = s_Dt / std::max(0.001f, m_OverlayFade);
    m_OverlayAlpha = s_OverlayOn ? std::min(1.f, m_OverlayAlpha + s_Step)
                                 : std::max(0.f, m_OverlayAlpha - s_Step);

    UpdateMusicFade(s_Dt);
}

void DramaticAimingShift::BeginEffect() {
    m_Active      = true;
    m_ActiveTime  = 0.f;
    m_TimeApplied = false;

    if (m_PlayMusic) {
        PlayMusic();
        const auto s_Start = FindAudio(L"slowmo_start");
        if (!s_Start.empty()) PlayOneShot(L"DASfxIn", s_Start);
    }
}

void DramaticAimingShift::EndEffect() {
    if (!m_Active)
        return;

    m_Active = false;
    if (m_TimeApplied) {
        SetTimeScale(1.f);
        m_TimeApplied = false;
    }

    StopMusic();
    const auto s_End = FindAudio(L"slowmo_end");
    if (m_PlayMusic && !s_End.empty()) PlayOneShot(L"DASfxOut", s_End);
}

// ---------------------------------------------------------------------------
// Game-facing shim #1: game time multiplier.
// The original mod set "m_fLevelSetupMultiplier" on a ZDebugHM5GameTimeMultiplierEnt.
// ---------------------------------------------------------------------------
void DramaticAimingShift::SetTimeScale(float p_Scale) {
    if (Globals::GameTimeManager)
        Globals::GameTimeManager->m_fGameTimeMultiplier = p_Scale;
}

// ---------------------------------------------------------------------------
// Audio
// ---------------------------------------------------------------------------
std::wstring DramaticAimingShift::FindAudio(const wchar_t* p_BaseName) const {
    static const wchar_t* s_Ext[] = { L".mp3", L".wav", L".wma" };
    const std::wstring s_Dir = ModuleDir();
    for (auto* s_E : s_Ext) {
        std::wstring s_Path = s_Dir + p_BaseName + s_E;
        if (GetFileAttributesW(s_Path.c_str()) != INVALID_FILE_ATTRIBUTES)
            return s_Path;
    }
    return L"";
}

void DramaticAimingShift::PlayMusic() {
    if (!m_PlayMusic)
        return;

    const auto s_File = FindAudio(L"ave_maria");
    if (s_File.empty()) {
        Logger::Warn("DramaticAimingShift: ave_maria.(mp3|wav|wma) not found next to the plugin DLL.");
        return;
    }

    Mci(L"close DAMusic");
    Mci(L"open \"" + s_File + L"\" alias DAMusic");
    m_MusicGain     = 0.f;
    m_MusicPlaying  = true;
    m_MusicStopping = false;
    Mci(L"setaudio DAMusic volume to 0");
    Mci(L"play DAMusic from 0");
}

void DramaticAimingShift::StopMusic() {
    if (m_MusicPlaying)
        m_MusicStopping = true; // faded out in UpdateMusicFade, then closed
}

void DramaticAimingShift::PlayOneShot(const wchar_t* p_Alias, const std::wstring& p_File) {
    const std::wstring s_Alias = p_Alias;
    Mci(L"close " + s_Alias);
    Mci(L"open \"" + p_File + L"\" alias " + s_Alias);
    Mci(L"setaudio " + s_Alias + L" volume to " + std::to_wstring(static_cast<int>(m_MusicVolume * 1000)));
    Mci(L"play " + s_Alias + L" from 0");
}

void DramaticAimingShift::UpdateMusicFade(float p_Dt) {
    if (!m_MusicPlaying)
        return;

    const float s_Step = p_Dt / std::max(0.001f, m_MusicFade);
    m_MusicGain = m_MusicStopping ? std::max(0.f, m_MusicGain - s_Step)
                                  : std::min(1.f, m_MusicGain + s_Step);

    Mci(L"setaudio DAMusic volume to " + std::to_wstring(static_cast<int>(m_MusicGain * m_MusicVolume * 1000)));

    if (m_MusicStopping && m_MusicGain <= 0.f) {
        Mci(L"stop DAMusic");
        Mci(L"close DAMusic");
        m_MusicPlaying  = false;
        m_MusicStopping = false;
    }
}

// ---------------------------------------------------------------------------
// Grey overlay + settings UI
// ---------------------------------------------------------------------------
void DramaticAimingShift::OnDrawUI(bool) {
    if (m_OverlayAlpha <= 0.001f)
        return;

    const ImVec2 s_Size = ImGui::GetIO().DisplaySize;
    const int    s_A    = static_cast<int>(m_OverlayAlpha * m_OverlayAlphaMax * 255.f);
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), s_Size, IM_COL32(128, 128, 128, s_A));
}

void DramaticAimingShift::OnDrawMenu() {
    if (ImGui::BeginMenu("Dramatic Aiming (Shift)")) {
        ImGui::Checkbox("Enabled", &m_Enabled);
        ImGui::Checkbox("Require Shift while aiming", &m_RequireShift);
        ImGui::Checkbox("Play Ave Maria / slow-mo sounds", &m_PlayMusic);
        ImGui::SliderFloat("Time scale", &m_TimeScale, 0.05f, 1.f);
        ImGui::SliderFloat("Overlay strength", &m_OverlayAlphaMax, 0.f, 1.f);
        ImGui::SliderFloat("Music volume", &m_MusicVolume, 0.f, 1.f);
        ImGui::Separator();
        ImGui::Text("aimPin=%d  exitIgnored=%d  shift=%d  rmb=%d",
                    (int)m_AimPin.load(), (int)m_ExitIgnored.load(), (int)IsShiftHeld(),
                    (int)((GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0));
        ImGui::Text("active=%d  timeApplied=%d  gameScale=%.2f", (int)m_Active, (int)m_TimeApplied,
                    Globals::GameTimeManager ? Globals::GameTimeManager->m_fGameTimeMultiplier : -1.f);
        ImGui::EndMenu();
    }
}

DEFINE_ZHM_PLUGIN(DramaticAimingShift)
