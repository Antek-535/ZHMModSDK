#pragma once

#include <IPluginInterface.h>
#include <Glacier/SGameUpdateEvent.h>

class FocusForAll : public IPluginInterface {
public:
    void Init() override;
    void OnEngineInitialized() override;
    void OnFrameUpdate(const SGameUpdateEvent& p_UpdateEvent) override;

private:
    void SetTimeScale(float p_Scale);

    float m_CurrentScale = 1.0f;
    float m_FocusScale = 0.30f;   // time scale while focusing (1.0 = normal)
    float m_BlendSpeed = 6.0f;    // higher = snappier transition
    bool m_WasActive = false;
};

DECLARE_ZHM_PLUGIN(FocusForAll)
