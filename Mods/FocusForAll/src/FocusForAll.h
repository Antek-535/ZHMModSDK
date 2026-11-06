#pragma once

#include "IPluginInterface.h"

class FocusForAll : public IPluginInterface {
public:
    ~FocusForAll() override;

    void OnEngineInitialized() override;

private:
    void OnFrameUpdate(const SGameUpdateEvent& p_UpdateEvent);

    float m_CurrentScale = 1.0f;
    float m_FocusScale = 0.30f;  // time scale while focusing (1.0 = normal)
    float m_BlendSpeed = 6.0f;   // higher = snappier transition
    bool m_Touched = false;      // true while we are controlling the multiplier
};

DECLARE_ZHM_PLUGIN(FocusForAll)
