#pragma once

#include <IPluginInterface.h>
#include <Glacier/ZGameLoopManager.h>
#include <chrono>
#include <string>
#include <vector>

class InstinctSounds : public IPluginInterface {
public:
    ~InstinctSounds() override;
    void Init() override;
    void OnEngineInitialized() override;
    void OnDrawMenu() override;
    void OnDrawUI(bool p_HasFocus) override;

private:
    enum class FadeState { None, In, Out };

    void OnFrameUpdate(const SGameUpdateEvent& p_UpdateEvent);
    void ScanFolder();
    void GeneratePresets();
    void StartSound();
    void StopSound();
    void CloseSound();
    void SetMciVolume(float p_Vol);
    void LoadSettings();
    void SaveSettings();
    std::string BrowseForFile();

    bool m_ShowUI = false;
    bool m_Held = false;
    bool m_SoundActive = false;

    // Fade state
    FadeState m_Fade = FadeState::None;
    std::chrono::steady_clock::time_point m_FadeStart;
    float m_FadeFromVol = 0.f;
    float m_CurVol = 0.f;

    // Settings
    int m_SelectedIndex = 0;
    std::string m_CustomPath;
    bool m_UseCustom = false;
    float m_Volume = 0.8f;
    bool m_FadeOut = true;
    int m_FadeOutMs = 400;
    bool m_FadeIn = false;
    int m_FadeInMs = 150;
    int m_Key = 0x10; // VK_SHIFT

    std::vector<std::string> m_Files; // full paths
    std::string m_Folder;
};

DECLARE_ZHM_PLUGIN(InstinctSounds)
