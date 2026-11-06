#include "InstinctSounds.h"

#include <Logging.h>
#include <Globals.h>
#include <imgui.h>

#include <windows.h>
#include <mmsystem.h>
#include <commdlg.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "comdlg32.lib")

namespace fs = std::filesystem;

static bool g_Open = false;

static void Mci(const wchar_t* p_Cmd) {
    mciSendStringW(p_Cmd, nullptr, 0, nullptr);
}

static std::string ModFolder() {
    char s_Path[MAX_PATH];
    HMODULE s_Mod = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&ModFolder), &s_Mod);
    GetModuleFileNameA(s_Mod, s_Path, MAX_PATH);
    return (fs::path(s_Path).parent_path() / "InstinctSounds").string();
}

// ---- tiny WAV writer for built-in presets ----
static void WriteWav(const fs::path& p_Path, const std::vector<int16_t>& p_Samples, int p_Rate = 44100) {
    std::ofstream f(p_Path, std::ios::binary);
    uint32_t dataSize = (uint32_t)(p_Samples.size() * 2);
    uint32_t chunk = 36 + dataSize, fmtSize = 16, byteRate = p_Rate * 2;
    uint16_t fmt = 1, ch = 1, align = 2, bits = 16;
    uint32_t rate = p_Rate;
    f.write("RIFF", 4); f.write((char*)&chunk, 4); f.write("WAVE", 4);
    f.write("fmt ", 4); f.write((char*)&fmtSize, 4); f.write((char*)&fmt, 2);
    f.write((char*)&ch, 2); f.write((char*)&rate, 4); f.write((char*)&byteRate, 4);
    f.write((char*)&align, 2); f.write((char*)&bits, 2);
    f.write("data", 4); f.write((char*)&dataSize, 4);
    f.write((char*)p_Samples.data(), dataSize);
}

void InstinctSounds::GeneratePresets() {
    const int rate = 44100;
    const double tau = 6.283185307179586;

    if (!fs::exists(fs::path(m_Folder) / "Preset - Low Hum.wav")) {
        std::vector<int16_t> s(rate * 4);
        for (size_t i = 0; i < s.size(); ++i) {
            double t = (double)i / rate;
            double v = 0.5 * std::sin(tau * 55.0 * t) + 0.25 * std::sin(tau * 110.0 * t)
                     + 0.1 * std::sin(tau * 82.5 * t);
            s[i] = (int16_t)(v * 0.6 * 32767);
        }
        WriteWav(fs::path(m_Folder) / "Preset - Low Hum.wav", s);
    }
    if (!fs::exists(fs::path(m_Folder) / "Preset - Heartbeat.wav")) {
        std::vector<int16_t> s(rate * 1, 0);
        auto thump = [&](double start, double amp) {
            for (int i = 0; i < rate / 6; ++i) {
                double t = (double)i / rate;
                double env = std::exp(-t * 28.0);
                size_t idx = (size_t)(start * rate) + i;
                if (idx < s.size()) s[idx] += (int16_t)(std::sin(tau * 60.0 * t) * env * amp * 32767);
            }
        };
        thump(0.00, 0.8);
        thump(0.22, 0.55);
        WriteWav(fs::path(m_Folder) / "Preset - Heartbeat.wav", s);
    }
    if (!fs::exists(fs::path(m_Folder) / "Preset - Pulse Sweep.wav")) {
        std::vector<int16_t> s(rate * 2);
        double phase = 0;
        for (size_t i = 0; i < s.size(); ++i) {
            double t = (double)i / rate;
            double freq = 200.0 + 120.0 * std::sin(tau * 0.5 * t);
            phase += tau * freq / rate;
            double pulse = 0.5 + 0.5 * std::sin(tau * 2.0 * t);
            s[i] = (int16_t)(std::sin(phase) * (0.25 + 0.35 * pulse) * 32767);
        }
        WriteWav(fs::path(m_Folder) / "Preset - Pulse Sweep.wav", s);
    }
}

void InstinctSounds::ScanFolder() {
    m_Files.clear();
    std::error_code ec;
    for (auto& e : fs::directory_iterator(m_Folder, ec)) {
        if (!e.is_regular_file()) continue;
        auto ext = e.path().extension().string();
        for (auto& c : ext) c = (char)tolower(c);
        if (ext == ".wav" || ext == ".mp3")
            m_Files.push_back(e.path().string());
    }
    if (m_SelectedIndex >= (int)m_Files.size()) m_SelectedIndex = 0;
}

InstinctSounds::~InstinctSounds() {
    CloseSound();
    const ZMemberDelegate<InstinctSounds, void(const SGameUpdateEvent&)> s_Delegate(this, &InstinctSounds::OnFrameUpdate);
    Globals::GameLoopManager->UnregisterFrameUpdate(s_Delegate, 1, EUpdateMode::eUpdatePlayMode);
}

void InstinctSounds::Init() {
    m_Folder = ModFolder();
    std::error_code ec;
    fs::create_directories(m_Folder, ec);
    GeneratePresets();
    ScanFolder();
    LoadSettings();
}

void InstinctSounds::OnEngineInitialized() {
    const ZMemberDelegate<InstinctSounds, void(const SGameUpdateEvent&)> s_Delegate(this, &InstinctSounds::OnFrameUpdate);
    Globals::GameLoopManager->RegisterFrameUpdate(s_Delegate, 1, EUpdateMode::eUpdatePlayMode);
}

void InstinctSounds::SetMciVolume(float p_Vol) {
    m_CurVol = std::clamp(p_Vol, 0.f, 1.f);
    std::wstring s_Cmd = L"setaudio instinctsnd volume to " + std::to_wstring((int)(m_CurVol * 1000));
    Mci(s_Cmd.c_str());
}

void InstinctSounds::CloseSound() {
    if (g_Open) {
        Mci(L"stop instinctsnd");
        Mci(L"close instinctsnd");
        g_Open = false;
    }
    m_SoundActive = false;
    m_Fade = FadeState::None;
}

void InstinctSounds::StartSound() {
    std::string s_Path = m_UseCustom ? m_CustomPath
        : (m_Files.empty() ? "" : m_Files[m_SelectedIndex]);
    if (s_Path.empty()) return;

    CloseSound();

    std::wstring s_Open = L"open \"" + fs::path(s_Path).wstring() + L"\" type mpegvideo alias instinctsnd";
    if (mciSendStringW(s_Open.c_str(), nullptr, 0, nullptr) != 0) {
        Logger::Error("InstinctSounds: failed to open {}", s_Path);
        return;
    }
    g_Open = true;

    SetMciVolume(m_FadeIn ? 0.f : m_Volume);
    Mci(L"play instinctsnd repeat");
    m_SoundActive = true;

    if (m_FadeIn) {
        m_Fade = FadeState::In;
        m_FadeStart = std::chrono::steady_clock::now();
    }
}

void InstinctSounds::StopSound() {
    if (!g_Open || !m_SoundActive) return;
    m_SoundActive = false;

    if (m_FadeOut && m_FadeOutMs > 0) {
        m_FadeFromVol = m_CurVol;
        m_Fade = FadeState::Out;
        m_FadeStart = std::chrono::steady_clock::now();
    } else {
        CloseSound();
    }
}

void InstinctSounds::OnFrameUpdate(const SGameUpdateEvent&) {
    // Fade handling
    if (m_Fade != FadeState::None) {
        float s_Ms = std::chrono::duration<float, std::milli>(
            std::chrono::steady_clock::now() - m_FadeStart).count();

        if (m_Fade == FadeState::In) {
            float t = std::min(s_Ms / (float)std::max(m_FadeInMs, 1), 1.f);
            SetMciVolume(m_Volume * t);
            if (t >= 1.f) m_Fade = FadeState::None;
        } else {
            float t = std::min(s_Ms / (float)std::max(m_FadeOutMs, 1), 1.f);
            SetMciVolume(m_FadeFromVol * (1.f - t));
            if (t >= 1.f) CloseSound();
        }
    }

    // Key polling (don't trigger while typing in a UI box)
    bool s_Down = (GetAsyncKeyState(m_Key) & 0x8000) != 0 && !ImGui::GetIO().WantCaptureKeyboard;

    if (s_Down && !m_Held) StartSound();
    else if (!s_Down && m_Held) StopSound();
    m_Held = s_Down;
}

std::string InstinctSounds::BrowseForFile() {
    wchar_t s_File[MAX_PATH] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = L"Audio files\0*.wav;*.mp3\0All files\0*.*\0";
    ofn.lpstrFile = s_File;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return {};
    return fs::path(s_File).string();
}

void InstinctSounds::OnDrawMenu() {
    if (ImGui::Button("INSTINCT SOUNDS")) m_ShowUI = !m_ShowUI;
}

void InstinctSounds::OnDrawUI(bool) {
    if (!m_ShowUI) return;
    ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Instinct Sounds", &m_ShowUI)) { ImGui::End(); return; }

    bool s_Changed = false;

    ImGui::TextUnformatted("Sound");
    if (ImGui::RadioButton("Preset / folder", !m_UseCustom)) { m_UseCustom = false; s_Changed = true; }
    ImGui::SameLine();
    if (ImGui::RadioButton("Custom file", m_UseCustom)) { m_UseCustom = true; s_Changed = true; }

    if (!m_UseCustom) {
        std::string s_Cur = m_Files.empty() ? "(none)" : fs::path(m_Files[m_SelectedIndex]).filename().string();
        if (ImGui::BeginCombo("##snd", s_Cur.c_str())) {
            for (int i = 0; i < (int)m_Files.size(); ++i)
                if (ImGui::Selectable(fs::path(m_Files[i]).filename().string().c_str(), i == m_SelectedIndex)) {
                    m_SelectedIndex = i; s_Changed = true;
                }
            ImGui::EndCombo();
        }
        if (ImGui::Button("Rescan folder")) ScanFolder();
    } else {
        ImGui::TextWrapped("%s", m_CustomPath.empty() ? "(no file chosen)" : m_CustomPath.c_str());
        if (ImGui::Button("Browse...")) {
            auto s_P = BrowseForFile();
            if (!s_P.empty()) { m_CustomPath = s_P; s_Changed = true; }
        }
    }

    ImGui::Separator();
    if (ImGui::SliderFloat("Volume", &m_Volume, 0.0f, 1.0f)) {
        s_Changed = true;
        if (g_Open && m_SoundActive && m_Fade == FadeState::None) SetMciVolume(m_Volume);
    }

    ImGui::Separator();
    ImGui::TextUnformatted("When instinct is released:");
    if (ImGui::RadioButton("Stop instantly", !m_FadeOut)) { m_FadeOut = false; s_Changed = true; }
    ImGui::SameLine();
    if (ImGui::RadioButton("Fade out", m_FadeOut)) { m_FadeOut = true; s_Changed = true; }
    if (m_FadeOut) s_Changed |= ImGui::SliderInt("Fade-out (ms)", &m_FadeOutMs, 50, 3000);

    s_Changed |= ImGui::Checkbox("Fade in on activate", &m_FadeIn);
    if (m_FadeIn) s_Changed |= ImGui::SliderInt("Fade-in (ms)", &m_FadeInMs, 50, 2000);

    ImGui::Separator();
    ImGui::TextUnformatted("Instinct key (must match your in-game binding)");
    static const struct { const char* n; int vk; } s_Keys[] = {
        {"Left Shift / Shift", 0x10}, {"Left Ctrl", 0xA2}, {"Left Alt", 0xA4},
        {"Tab", 0x09}, {"Caps Lock", 0x14}, {"Q", 'Q'}, {"E", 'E'}, {"F", 'F'}, {"V", 'V'}, {"X", 'X'},
        {"Mouse Middle", 0x04}, {"Mouse X1", 0x05}, {"Mouse X2", 0x06}
    };
    const char* s_KeyName = "Custom";
    for (auto& k : s_Keys) if (k.vk == m_Key) s_KeyName = k.n;
    if (ImGui::BeginCombo("##key", s_KeyName)) {
        for (auto& k : s_Keys)
            if (ImGui::Selectable(k.n, k.vk == m_Key)) { m_Key = k.vk; s_Changed = true; }
        ImGui::EndCombo();
    }

    if (ImGui::Button("Test sound")) { StartSound(); }
    ImGui::SameLine();
    if (ImGui::Button("Stop test")) { StopSound(); }

    if (s_Changed) SaveSettings();
    ImGui::End();
}

void InstinctSounds::SaveSettings() {
    std::ofstream f(fs::path(m_Folder) / "settings.txt");
    f << m_SelectedIndex << "\n" << m_UseCustom << "\n" << m_CustomPath << "\n" << m_Volume << "\n"
      << m_FadeOut << "\n" << m_FadeOutMs << "\n" << m_FadeIn << "\n" << m_FadeInMs << "\n" << m_Key << "\n";
}

void InstinctSounds::LoadSettings() {
    std::ifstream f(fs::path(m_Folder) / "settings.txt");
    if (!f) return;
    f >> m_SelectedIndex >> m_UseCustom;
    f.ignore();
    std::getline(f, m_CustomPath);
    f >> m_Volume >> m_FadeOut >> m_FadeOutMs >> m_FadeIn >> m_FadeInMs >> m_Key;
    if (m_SelectedIndex < 0 || m_SelectedIndex >= (int)m_Files.size()) m_SelectedIndex = 0;
}

DEFINE_ZHM_PLUGIN(InstinctSounds)
