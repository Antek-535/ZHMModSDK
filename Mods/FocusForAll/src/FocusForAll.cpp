#include "FocusForAll.h"

#include <algorithm>
#include <chrono>
#include <Windows.h>

#include <Logging.h>
#include <Globals.h>
#include <Glacier/ZGameTimeManager.h>

void FocusForAll::Init() {
    Logger::Info("FocusForAll loaded: hold SHIFT + aim (right mouse) to slow time with any weapon.");
}

void FocusForAll::OnEngineInitialized() {}

void FocusForAll::SetTimeScale(float p_Scale) {
    // !! VERIFY against your ZHMModSDK version !!
    // ZGameTimeManager exposes time multipliers; check Glacier/ZGameTimeManager.h
    // for the exact member names in the SDK revision you build against.
    if (Globals::GameTimeManager) {
        Globals::GameTimeManager->m_fTimeMultiplier0 = p_Scale;
    }
}

void FocusForAll::OnFrameUpdate(const SGameUpdateEvent&) {
    // Real (unscaled) frame delta so blending doesn't depend on the slowdown itself.
    using clock = std::chrono::steady_clock;
    static auto s_Last = clock::now();
    const auto s_Now = clock::now();
    const float s_Dt = std::chrono::duration<float>(s_Now - s_Last).count();
    s_Last = s_Now;

    const bool s_InMission = SDK()->GetLocalPlayer().m_pInterfaceRef != nullptr;

    const bool s_Shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool s_Aim = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    const bool s_Active = s_InMission && s_Shift && s_Aim;

    const float s_Target = s_Active ? m_FocusScale : 1.0f;
    const float s_T = std::clamp(m_BlendSpeed * s_Dt, 0.0f, 1.0f);
    m_CurrentScale += (s_Target - m_CurrentScale) * s_T;

    // Only touch the multiplier while we are (or were just) active, so we don't
    // fight other mods or the game's own time effects the rest of the time.
    if (s_Active || std::abs(m_CurrentScale - 1.0f) > 0.001f) {
        SetTimeScale(m_CurrentScale);
        m_WasActive = true;
    } else if (m_WasActive) {
        SetTimeScale(1.0f);
        m_CurrentScale = 1.0f;
        m_WasActive = false;
    }
}

DEFINE_ZHM_PLUGIN(FocusForAll)
