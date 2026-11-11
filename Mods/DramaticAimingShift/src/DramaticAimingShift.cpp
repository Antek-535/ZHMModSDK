#include "DramaticAimingShift.h"

#include <Hooks.h>
#include <Logging.h>
#include <Globals.h>
#include <Functions.h>

#include <Glacier/ZGameLoopManager.h>
#include <Glacier/ZGameTime.h>
#include <Glacier/ZString.h>
#include <Glacier/ZHitman5.h>
#include <Glacier/ZModule.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <vector>

#include <imgui.h>
#include <wrl/client.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <d3d12.h>
#include <mmsystem.h>
#include <shellapi.h>
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "shell32.lib")

namespace fs = std::filesystem;

static constexpr char kSection[] = "DramaticAimingShift";

namespace {

// Glacier pin ids are the CRC32 of their name.
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

// Stable, always-writable place for our own files (LUT cache, saved sound choice).
std::wstring ConfigDir() {
    wchar_t s_Buf[MAX_PATH] = {};
    const DWORD s_Len = GetEnvironmentVariableW(L"APPDATA", s_Buf, MAX_PATH);
    if (s_Len == 0 || s_Len >= MAX_PATH) return L"";
    return std::wstring(s_Buf) + L"\\DramaticAimingShift\\";
}

std::wstring Utf8ToWide(const std::string& p_Str) {
    if (p_Str.empty()) return L"";
    const int s_Len = MultiByteToWideChar(CP_UTF8, 0, p_Str.c_str(), -1, nullptr, 0);
    std::wstring s_Out(s_Len > 0 ? s_Len - 1 : 0, L'\0');
    if (s_Len > 1) MultiByteToWideChar(CP_UTF8, 0, p_Str.c_str(), -1, s_Out.data(), s_Len);
    return s_Out;
}

std::string WideToUtf8(const std::wstring& p_Str) {
    if (p_Str.empty()) return "";
    const int s_Len = WideCharToMultiByte(CP_UTF8, 0, p_Str.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s_Out(s_Len > 0 ? s_Len - 1 : 0, '\0');
    if (s_Len > 1) WideCharToMultiByte(CP_UTF8, 0, p_Str.c_str(), -1, s_Out.data(), s_Len, nullptr, nullptr);
    return s_Out;
}

bool GameWindowFocused() {
    DWORD s_Pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &s_Pid);
    return s_Pid == GetCurrentProcessId();
}

void Mci(const std::wstring& p_Cmd) {
    mciSendStringW(p_Cmd.c_str(), nullptr, 0, nullptr);
}

void SendKey(int p_Vk, bool p_Up) {
    INPUT s_In = {};
    s_In.type = INPUT_KEYBOARD;
    s_In.ki.wVk = static_cast<WORD>(p_Vk);
    s_In.ki.wScan = static_cast<WORD>(MapVirtualKeyW(static_cast<UINT>(p_Vk), MAPVK_VK_TO_VSC));
    s_In.ki.dwFlags = p_Up ? KEYEVENTF_KEYUP : 0;
    SendInput(1, &s_In, sizeof(INPUT));
}

struct SKeyChoice { const char* Name; int Vk; };
const SKeyChoice kKeys[] = {
    {"Left Ctrl", VK_LCONTROL}, {"Left Alt", VK_LMENU}, {"Tab", VK_TAB}, {"Q", 'Q'}, {"E", 'E'},
    {"F", 'F'}, {"X", 'X'}, {"V", 'V'}, {"Z", 'Z'}, {"G", 'G'}, {"B", 'B'},
};
constexpr int kKeyCount = static_cast<int>(sizeof(kKeys) / sizeof(kKeys[0]));

} // namespace

// ---------------------------------------------------------------------------
// Overlay: LUT helpers copied from BetterInstinct (same technique, same safety checks).
// ---------------------------------------------------------------------------

namespace {

constexpr uint64_t kInstinctLutId = 0x008C1F5C7305A978ULL;
constexpr UINT kLutDim = 64;

bool LutSafeReadPtr(const void* p_Addr, void** p_Out) {
    __try {
        *p_Out = *reinterpret_cast<void* const*>(p_Addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// True when the object's vtable lives inside a d3d12 module (cheap check before calling QueryInterface on it).
bool LutIsD3DObject(void* p_Obj) {
    void* s_Vtbl = nullptr;
    if (!LutSafeReadPtr(p_Obj, &s_Vtbl) || !s_Vtbl) return false;

    HMODULE s_Module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(s_Vtbl), &s_Module) || !s_Module) {
        return false;
    }

    wchar_t s_Name[MAX_PATH] = {};
    if (!GetModuleFileNameW(s_Module, s_Name, MAX_PATH)) return false;
    for (wchar_t* c = s_Name; *c; ++c) *c = static_cast<wchar_t>(std::towlower(*c));
    return std::wcsstr(s_Name, L"d3d12") != nullptr;
}

ID3D12Resource* LutSafeQuery(void* p_Obj) {
    __try {
        ID3D12Resource* s_Res = nullptr;
        if (SUCCEEDED(reinterpret_cast<IUnknown*>(p_Obj)->QueryInterface(__uuidof(ID3D12Resource),
                                                                         reinterpret_cast<void**>(&s_Res)))) {
            return s_Res;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return nullptr;
}

// Returns an AddRef'd ID3D12Resource when p_Cand is the 64x64 2D texture we are looking for.
ID3D12Resource* LutTryCandidate(void* p_Cand) {
    if (!p_Cand || (reinterpret_cast<uintptr_t>(p_Cand) & 7) || reinterpret_cast<uintptr_t>(p_Cand) < 0x10000) return nullptr;
    if (!LutIsD3DObject(p_Cand)) return nullptr;

    ID3D12Resource* s_Res = LutSafeQuery(p_Cand);
    if (!s_Res) return nullptr;

    const D3D12_RESOURCE_DESC s_Desc = s_Res->GetDesc();
    if (s_Desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && s_Desc.Width == kLutDim && s_Desc.Height == kLutDim &&
        s_Desc.DepthOrArraySize == 1) {
        return s_Res;
    }

    s_Res->Release();
    return nullptr;
}

// The loaded TEXT resource wraps the GPU texture somewhere inside it; look through the object (and one level of
// pointers inside it) without needing to know the exact layout.
ID3D12Resource* LutProbe(void* p_Data, std::string& p_How) {
    if (!p_Data) return nullptr;

    if (ID3D12Resource* s_Res = LutTryCandidate(p_Data)) {
        p_How = "resource data is the texture";
        return s_Res;
    }

    char s_Buf[96];
    for (size_t s_Off = 0; s_Off < 0x100; s_Off += 8) {
        void* s_Q = nullptr;
        if (!LutSafeReadPtr(static_cast<char*>(p_Data) + s_Off, &s_Q)) break;
        if (ID3D12Resource* s_Res = LutTryCandidate(s_Q)) {
            std::snprintf(s_Buf, sizeof(s_Buf), "texture at resource data + 0x%zX", s_Off);
            p_How = s_Buf;
            return s_Res;
        }
    }

    for (size_t s_Off = 0; s_Off < 0x100; s_Off += 8) {
        void* s_Q = nullptr;
        if (!LutSafeReadPtr(static_cast<char*>(p_Data) + s_Off, &s_Q)) break;
        if (!s_Q || (reinterpret_cast<uintptr_t>(s_Q) & 7) || reinterpret_cast<uintptr_t>(s_Q) < 0x10000) continue;

        for (size_t s_Off2 = 0; s_Off2 < 0x100; s_Off2 += 8) {
            void* s_Q2 = nullptr;
            if (!LutSafeReadPtr(static_cast<char*>(s_Q) + s_Off2, &s_Q2)) break;
            if (ID3D12Resource* s_Res = LutTryCandidate(s_Q2)) {
                std::snprintf(s_Buf, sizeof(s_Buf), "texture at resource data + 0x%zX -> + 0x%zX", s_Off, s_Off2);
                p_How = s_Buf;
                return s_Res;
            }
        }
    }

    return nullptr;
}

} // namespace

// GPU-side state for the LUT texture (kept out of the header so it doesn't need the D3D12 headers).
struct SLutGpu {
    Microsoft::WRL::ComPtr<ID3D12Resource> m_Res;
    Microsoft::WRL::ComPtr<ID3D12Device> m_Dev;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_Queue;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_Alloc;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_List;
    Microsoft::WRL::ComPtr<ID3D12Fence> m_Fence;
    HANDLE m_Event = nullptr;
    UINT64 m_FenceValue = 0;
    D3D12_RESOURCE_DESC m_Desc{};
    uint32_t m_Index = 0;      // index in the resource container
    void* m_Data = nullptr;    // resourceData pointer at the time we found it (changes when the game reloads it)
    std::vector<uint8_t> m_Base; // vanilla LUT, mip 0, always RGBA order
    bool m_Bgra = false;
    bool m_NoAlpha = false;
    std::string m_How;

    ~SLutGpu() {
        if (m_Event) CloseHandle(m_Event);
    }
};

namespace {

bool LutFormatInfo(DXGI_FORMAT p_Format, bool& p_Bgra, bool& p_NoAlpha) {
    p_NoAlpha = false;
    switch (p_Format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        p_Bgra = false;
        return true;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        p_Bgra = true;
        return true;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        p_Bgra = true;
        p_NoAlpha = true;
        return true;
    default:
        return false;
    }
}

bool LutBegin(SLutGpu& p_Gpu) {
    return SUCCEEDED(p_Gpu.m_Alloc->Reset()) && SUCCEEDED(p_Gpu.m_List->Reset(p_Gpu.m_Alloc.Get(), nullptr));
}

// Submits the recorded list on our own queue and waits for it (the LUT is tiny, so this is well under a millisecond).
bool LutRun(SLutGpu& p_Gpu) {
    if (FAILED(p_Gpu.m_List->Close())) return false;

    ID3D12CommandList* s_Lists[] = {p_Gpu.m_List.Get()};
    p_Gpu.m_Queue->ExecuteCommandLists(1, s_Lists);

    const UINT64 s_Value = ++p_Gpu.m_FenceValue;
    if (FAILED(p_Gpu.m_Queue->Signal(p_Gpu.m_Fence.Get(), s_Value))) return false;

    if (p_Gpu.m_Fence->GetCompletedValue() < s_Value) {
        if (FAILED(p_Gpu.m_Fence->SetEventOnCompletion(s_Value, p_Gpu.m_Event))) return false;
        if (WaitForSingleObject(p_Gpu.m_Event, 3000) != WAIT_OBJECT_0) return false;
    }
    return true;
}

D3D12_RESOURCE_BARRIER LutBarrier(ID3D12Resource* p_Res, D3D12_RESOURCE_STATES p_From, D3D12_RESOURCE_STATES p_To) {
    D3D12_RESOURCE_BARRIER s_Barrier{};
    s_Barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    s_Barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    s_Barrier.Transition.pResource = p_Res;
    s_Barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    s_Barrier.Transition.StateBefore = p_From;
    s_Barrier.Transition.StateAfter = p_To;
    return s_Barrier;
}

// State the game leaves the texture in. 0 = shader resource (normal), 1 = common (if the first doesn't work).
D3D12_RESOURCE_STATES LutRestState(int p_Mode) {
    return p_Mode == 0 ? D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_COMMON;
}

D3D12_RESOURCE_DESC LutBufferDesc(UINT64 p_Size) {
    D3D12_RESOURCE_DESC s_Desc{};
    s_Desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    s_Desc.Width = p_Size;
    s_Desc.Height = 1;
    s_Desc.DepthOrArraySize = 1;
    s_Desc.MipLevels = 1;
    s_Desc.Format = DXGI_FORMAT_UNKNOWN;
    s_Desc.SampleDesc.Count = 1;
    s_Desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return s_Desc;
}

bool LutInit(SLutGpu& p_Gpu, ID3D12Resource* p_Res, std::string& p_Err) {
    p_Gpu.m_Res.Attach(p_Res); // takes the reference LutTryCandidate gave us
    p_Gpu.m_Desc = p_Res->GetDesc();

    if (!LutFormatInfo(p_Gpu.m_Desc.Format, p_Gpu.m_Bgra, p_Gpu.m_NoAlpha)) {
        p_Err = "the LUT texture has an unsupported pixel format (DXGI " + std::to_string(static_cast<int>(p_Gpu.m_Desc.Format)) + ")";
        return false;
    }

    if (FAILED(p_Res->GetDevice(IID_PPV_ARGS(&p_Gpu.m_Dev))) || !p_Gpu.m_Dev) {
        p_Err = "could not get the D3D12 device of the LUT texture";
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC s_QueueDesc{};
    s_QueueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(p_Gpu.m_Dev->CreateCommandQueue(&s_QueueDesc, IID_PPV_ARGS(&p_Gpu.m_Queue))) ||
        FAILED(p_Gpu.m_Dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&p_Gpu.m_Alloc))) ||
        FAILED(p_Gpu.m_Dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, p_Gpu.m_Alloc.Get(), nullptr,
                                              IID_PPV_ARGS(&p_Gpu.m_List))) ||
        FAILED(p_Gpu.m_Dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&p_Gpu.m_Fence)))) {
        p_Err = "could not create the D3D12 copy objects";
        return false;
    }
    p_Gpu.m_List->Close();

    p_Gpu.m_Event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!p_Gpu.m_Event) {
        p_Err = "could not create a wait event";
        return false;
    }
    return true;
}

// Reads mip 0 of the texture into m_Base (RGBA order).
bool LutReadback(SLutGpu& p_Gpu, int p_State, std::string& p_Err) {
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT s_Fp{};
    UINT s_Rows = 0;
    UINT64 s_RowSize = 0, s_Total = 0;
    p_Gpu.m_Dev->GetCopyableFootprints(&p_Gpu.m_Desc, 0, 1, 0, &s_Fp, &s_Rows, &s_RowSize, &s_Total);
    if (s_Total == 0) {
        p_Err = "texture footprint is empty";
        return false;
    }

    D3D12_HEAP_PROPERTIES s_Heap{};
    s_Heap.Type = D3D12_HEAP_TYPE_READBACK;
    s_Heap.CreationNodeMask = 1;
    s_Heap.VisibleNodeMask = 1;
    const D3D12_RESOURCE_DESC s_BufDesc = LutBufferDesc(s_Total);

    Microsoft::WRL::ComPtr<ID3D12Resource> s_Readback;
    if (FAILED(p_Gpu.m_Dev->CreateCommittedResource(&s_Heap, D3D12_HEAP_FLAG_NONE, &s_BufDesc, D3D12_RESOURCE_STATE_COPY_DEST,
                                                    nullptr, IID_PPV_ARGS(&s_Readback)))) {
        p_Err = "could not create the readback buffer";
        return false;
    }

    const D3D12_RESOURCE_STATES s_Rest = LutRestState(p_State);
    if (!LutBegin(p_Gpu)) {
        p_Err = "could not start the copy command list";
        return false;
    }

    D3D12_RESOURCE_BARRIER s_Barrier = LutBarrier(p_Gpu.m_Res.Get(), s_Rest, D3D12_RESOURCE_STATE_COPY_SOURCE);
    p_Gpu.m_List->ResourceBarrier(1, &s_Barrier);

    D3D12_TEXTURE_COPY_LOCATION s_Dst{};
    s_Dst.pResource = s_Readback.Get();
    s_Dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    s_Dst.PlacedFootprint = s_Fp;
    D3D12_TEXTURE_COPY_LOCATION s_Src{};
    s_Src.pResource = p_Gpu.m_Res.Get();
    s_Src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    s_Src.SubresourceIndex = 0;
    p_Gpu.m_List->CopyTextureRegion(&s_Dst, 0, 0, 0, &s_Src, nullptr);

    s_Barrier = LutBarrier(p_Gpu.m_Res.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, s_Rest);
    p_Gpu.m_List->ResourceBarrier(1, &s_Barrier);

    if (!LutRun(p_Gpu)) {
        p_Err = "the GPU copy did not finish";
        return false;
    }

    void* s_Mapped = nullptr;
    const D3D12_RANGE s_Range{0, static_cast<SIZE_T>(s_Total)};
    if (FAILED(s_Readback->Map(0, &s_Range, &s_Mapped)) || !s_Mapped) {
        p_Err = "could not read the readback buffer";
        return false;
    }

    const uint32_t s_W = static_cast<uint32_t>(p_Gpu.m_Desc.Width);
    const uint32_t s_H = p_Gpu.m_Desc.Height;
    p_Gpu.m_Base.assign(static_cast<size_t>(s_W) * s_H * 4, 255);
    for (uint32_t y = 0; y < s_H; ++y) {
        const uint8_t* s_Row = static_cast<const uint8_t*>(s_Mapped) + s_Fp.Offset + static_cast<size_t>(y) * s_Fp.Footprint.RowPitch;
        uint8_t* s_Out = &p_Gpu.m_Base[static_cast<size_t>(y) * s_W * 4];
        for (uint32_t x = 0; x < s_W; ++x) {
            const uint8_t* s_Px = s_Row + x * 4;
            s_Out[x * 4 + 0] = p_Gpu.m_Bgra ? s_Px[2] : s_Px[0];
            s_Out[x * 4 + 1] = s_Px[1];
            s_Out[x * 4 + 2] = p_Gpu.m_Bgra ? s_Px[0] : s_Px[2];
            s_Out[x * 4 + 3] = p_Gpu.m_NoAlpha ? 255 : s_Px[3];
        }
    }

    const D3D12_RANGE s_NoWrite{0, 0};
    s_Readback->Unmap(0, &s_NoWrite);
    return true;
}

// Writes an RGBA image (mip 0) into the texture; lower mips (if the texture has any) are box-filtered from it.
bool LutWrite(SLutGpu& p_Gpu, int p_State, const std::vector<uint8_t>& p_Rgba, std::string& p_Err) {
    const UINT s_Mips = p_Gpu.m_Desc.MipLevels ? p_Gpu.m_Desc.MipLevels : 1;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> s_Fp(s_Mips);
    std::vector<UINT> s_Rows(s_Mips);
    std::vector<UINT64> s_RowSize(s_Mips);
    UINT64 s_Total = 0;
    p_Gpu.m_Dev->GetCopyableFootprints(&p_Gpu.m_Desc, 0, s_Mips, 0, s_Fp.data(), s_Rows.data(), s_RowSize.data(), &s_Total);
    if (s_Total == 0) {
        p_Err = "texture footprint is empty";
        return false;
    }

    D3D12_HEAP_PROPERTIES s_Heap{};
    s_Heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    s_Heap.CreationNodeMask = 1;
    s_Heap.VisibleNodeMask = 1;
    const D3D12_RESOURCE_DESC s_BufDesc = LutBufferDesc(s_Total);

    Microsoft::WRL::ComPtr<ID3D12Resource> s_Upload;
    if (FAILED(p_Gpu.m_Dev->CreateCommittedResource(&s_Heap, D3D12_HEAP_FLAG_NONE, &s_BufDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                    nullptr, IID_PPV_ARGS(&s_Upload)))) {
        p_Err = "could not create the upload buffer";
        return false;
    }

    void* s_Mapped = nullptr;
    const D3D12_RANGE s_NoRead{0, 0};
    if (FAILED(s_Upload->Map(0, &s_NoRead, &s_Mapped)) || !s_Mapped) {
        p_Err = "could not map the upload buffer";
        return false;
    }

    uint32_t s_Mw = static_cast<uint32_t>(p_Gpu.m_Desc.Width);
    uint32_t s_Mh = p_Gpu.m_Desc.Height;
    std::vector<uint8_t> s_Cur = p_Rgba;
    for (UINT m = 0; m < s_Mips; ++m) {
        if (m > 0) {
            const uint32_t s_Pw = s_Mw, s_Ph = s_Mh;
            s_Mw = std::max(1u, s_Pw / 2);
            s_Mh = std::max(1u, s_Ph / 2);
            std::vector<uint8_t> s_Next(static_cast<size_t>(s_Mw) * s_Mh * 4);
            for (uint32_t y = 0; y < s_Mh; ++y) {
                for (uint32_t x = 0; x < s_Mw; ++x) {
                    const uint32_t s_X0 = std::min(x * 2, s_Pw - 1), s_X1 = std::min(x * 2 + 1, s_Pw - 1);
                    const uint32_t s_Y0 = std::min(y * 2, s_Ph - 1), s_Y1 = std::min(y * 2 + 1, s_Ph - 1);
                    for (int c = 0; c < 4; ++c) {
                        const uint32_t s_Sum = s_Cur[(static_cast<size_t>(s_Y0) * s_Pw + s_X0) * 4 + c] +
                                               s_Cur[(static_cast<size_t>(s_Y0) * s_Pw + s_X1) * 4 + c] +
                                               s_Cur[(static_cast<size_t>(s_Y1) * s_Pw + s_X0) * 4 + c] +
                                               s_Cur[(static_cast<size_t>(s_Y1) * s_Pw + s_X1) * 4 + c];
                        s_Next[(static_cast<size_t>(y) * s_Mw + x) * 4 + c] = static_cast<uint8_t>((s_Sum + 2) / 4);
                    }
                }
            }
            s_Cur = std::move(s_Next);
        }

        for (uint32_t y = 0; y < s_Mh; ++y) {
            uint8_t* s_Row = static_cast<uint8_t*>(s_Mapped) + s_Fp[m].Offset + static_cast<size_t>(y) * s_Fp[m].Footprint.RowPitch;
            for (uint32_t x = 0; x < s_Mw; ++x) {
                const uint8_t* s_Px = &s_Cur[(static_cast<size_t>(y) * s_Mw + x) * 4];
                s_Row[x * 4 + 0] = p_Gpu.m_Bgra ? s_Px[2] : s_Px[0];
                s_Row[x * 4 + 1] = s_Px[1];
                s_Row[x * 4 + 2] = p_Gpu.m_Bgra ? s_Px[0] : s_Px[2];
                s_Row[x * 4 + 3] = p_Gpu.m_NoAlpha ? 255 : s_Px[3];
            }
        }
    }
    s_Upload->Unmap(0, nullptr);

    const D3D12_RESOURCE_STATES s_Rest = LutRestState(p_State);
    if (!LutBegin(p_Gpu)) {
        p_Err = "could not start the copy command list";
        return false;
    }

    D3D12_RESOURCE_BARRIER s_Barrier = LutBarrier(p_Gpu.m_Res.Get(), s_Rest, D3D12_RESOURCE_STATE_COPY_DEST);
    p_Gpu.m_List->ResourceBarrier(1, &s_Barrier);

    for (UINT m = 0; m < s_Mips; ++m) {
        D3D12_TEXTURE_COPY_LOCATION s_Dst{};
        s_Dst.pResource = p_Gpu.m_Res.Get();
        s_Dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        s_Dst.SubresourceIndex = m;
        D3D12_TEXTURE_COPY_LOCATION s_Src{};
        s_Src.pResource = s_Upload.Get();
        s_Src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        s_Src.PlacedFootprint = s_Fp[m];
        p_Gpu.m_List->CopyTextureRegion(&s_Dst, 0, 0, 0, &s_Src, nullptr);
    }

    s_Barrier = LutBarrier(p_Gpu.m_Res.Get(), D3D12_RESOURCE_STATE_COPY_DEST, s_Rest);
    p_Gpu.m_List->ResourceBarrier(1, &s_Barrier);

    if (!LutRun(p_Gpu)) {
        p_Err = "the GPU copy did not finish";
        return false;
    }
    return true;
}

// Monochrome copy of the vanilla LUT in the chosen color. Strength 1 replaces the vanilla look completely.
std::vector<uint8_t> LutRecolor(const std::vector<uint8_t>& p_Base, const float* p_Color, float p_Strength, bool p_Keep, float p_Bright) {
    const float s_Max = std::max({p_Color[0], p_Color[1], p_Color[2], 0.001f});
    const float s_C[3] = {p_Color[0] / s_Max, p_Color[1] / s_Max, p_Color[2] / s_Max};
    const float s_LumC = 0.2126f * s_C[0] + 0.7152f * s_C[1] + 0.0722f * s_C[2];
    // "Keep brightness": dark colors (blue, red) would otherwise make a much darker picture than vanilla Instinct.
    const float s_Boost = p_Bright * (p_Keep ? 1.0f / std::max(s_LumC, 0.30f) : 1.0f);

    std::vector<uint8_t> s_Out(p_Base.size());
    for (size_t i = 0; i + 3 < p_Base.size(); i += 4) {
        const float s_R = p_Base[i + 0] / 255.0f, s_G = p_Base[i + 1] / 255.0f, s_B = p_Base[i + 2] / 255.0f;
        const float s_L = 0.2126f * s_R + 0.7152f * s_G + 0.0722f * s_B;
        const float s_In[3] = {s_R, s_G, s_B};
        for (int c = 0; c < 3; ++c) {
            const float s_Mono = std::clamp(s_L * s_C[c] * s_Boost, 0.0f, 1.0f);
            const float s_V = s_In[c] + (s_Mono - s_In[c]) * p_Strength;
            s_Out[i + c] = static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(s_V * 255.0f)), 0, 255));
        }
        s_Out[i + 3] = p_Base[i + 3];
    }
    return s_Out;
}

fs::path LutCachePath() {
    const std::wstring s_Dir = ConfigDir();
    // Prefer BetterInstinct's cache of the vanilla LUT if it has one, so we never mistake a recoloured LUT for vanilla.
    wchar_t s_App[MAX_PATH] = {};
    const DWORD s_AppLen = GetEnvironmentVariableW(L"APPDATA", s_App, MAX_PATH);
    if (s_AppLen > 0 && s_AppLen < MAX_PATH) {
        const fs::path s_Theirs = fs::path(std::wstring(s_App) + L"\\BetterInstinct\\instinct_lut_base.bin");
        std::error_code s_Ec;
        if (fs::exists(s_Theirs, s_Ec)) return s_Theirs;
    }
    return s_Dir.empty() ? fs::path() : fs::path(s_Dir) / L"instinct_lut_base.bin";
}

} // namespace

void DramaticAimingShift::SetLutStatus(int p_State, const std::string& p_Text) {
    m_LutState = p_State;
    std::lock_guard<std::mutex> s_Lock(m_LutMutex);
    m_LutStatus = p_Text;
}

void DramaticAimingShift::ReleaseLut() {
    delete m_Lut;
    m_Lut = nullptr;
    m_LutApplied = false;
    m_LutAppliedSig = 0;
    m_LutFailSig.store(0);
}

// Still the same resource, loaded and unchanged? (checked every frame, it is just a few reads)
bool DramaticAimingShift::LutStillValid() const {
    if (!m_Lut || !Globals::ResourceContainer || !*Globals::ResourceContainer) return false;
    auto& s_Resources = (*Globals::ResourceContainer)->m_resources;
    if (m_Lut->m_Index >= s_Resources.size()) return false;
    const auto& s_Info = s_Resources[m_Lut->m_Index];
    return s_Info.rid.GetID() == kInstinctLutId && s_Info.status == RESOURCE_STATUS_VALID && s_Info.resourceData == m_Lut->m_Data;
}

// Finds the LUT texture and gets the vanilla pixels. Returns true when m_Lut is ready to use.
bool DramaticAimingShift::EnsureLut() {
    if (m_Lut && LutStillValid()) return true;
    if (m_Lut) ReleaseLut(); // the game reloaded the texture (new mission): start again on the fresh one

    if (!Globals::ResourceContainer || !*Globals::ResourceContainer) {
        SetLutStatus(2, "The game's resource container is not available.");
        return false;
    }

    auto& s_Resources = (*Globals::ResourceContainer)->m_resources;
    const size_t s_Count = s_Resources.size();
    bool s_FoundButNotLoaded = false;

    for (size_t i = 0; i < s_Count; ++i) {
        const auto& s_Info = s_Resources[i];
        if (s_Info.rid.GetID() != kInstinctLutId) continue;

        if (s_Info.status != RESOURCE_STATUS_VALID || !s_Info.resourceData) {
            s_FoundButNotLoaded = true;
            continue;
        }

        std::string s_How;
        ID3D12Resource* s_Res = LutProbe(s_Info.resourceData, s_How);
        char s_Buf[256];
        if (!s_Res) {
            std::snprintf(s_Buf, sizeof(s_Buf),
                          "Found the Instinct LUT (resource #%zu, type 0x%08X, data %p) but no 64x64 D3D12 texture inside it.", i,
                          s_Info.resourceType, s_Info.resourceData);
            SetLutStatus(2, s_Buf);
            return false;
        }

        auto* s_Gpu = new SLutGpu();
        s_Gpu->m_Index = static_cast<uint32_t>(i);
        s_Gpu->m_Data = s_Info.resourceData;
        s_Gpu->m_How = s_How;

        std::string s_Err;
        if (!LutInit(*s_Gpu, s_Res, s_Err)) {
            delete s_Gpu;
            SetLutStatus(2, "Instinct LUT: " + s_Err + ".");
            return false;
        }

        // Vanilla pixels: from the cache file when there is one (so a plugin reload can never mistake our own
        // recolored texture for the vanilla one), otherwise read back from the GPU and cache.
        const size_t s_Bytes = static_cast<size_t>(s_Gpu->m_Desc.Width) * s_Gpu->m_Desc.Height * 4;
        const fs::path s_Cache = LutCachePath();
        bool s_Have = false;
        if (!s_Cache.empty()) {
            std::ifstream s_In(s_Cache, std::ios::binary);
            if (s_In) {
                std::vector<uint8_t> s_Data((std::istreambuf_iterator<char>(s_In)), std::istreambuf_iterator<char>());
                if (s_Data.size() == s_Bytes) {
                    s_Gpu->m_Base = std::move(s_Data);
                    s_Have = true;
                }
            }
        }
        if (!s_Have) {
            if (!LutReadback(*s_Gpu, m_LutBarrier, s_Err)) {
                delete s_Gpu;
                SetLutStatus(2, "Instinct LUT: " + s_Err + ".");
                return false;
            }
            if (!s_Cache.empty()) {
                std::error_code s_Ec;
                fs::create_directories(s_Cache.parent_path(), s_Ec);
                std::ofstream s_Out(s_Cache, std::ios::binary | std::ios::trunc);
                if (s_Out) s_Out.write(reinterpret_cast<const char*>(s_Gpu->m_Base.data()), static_cast<std::streamsize>(s_Gpu->m_Base.size()));
            }
        }

        m_Lut = s_Gpu;
        m_LutApplied = false;
        m_LutAppliedSig = 0;
        std::snprintf(s_Buf, sizeof(s_Buf), "Instinct LUT found (resource #%zu, %ux%u, DXGI %d, %s, %s).", i,
                      static_cast<unsigned>(s_Gpu->m_Desc.Width), s_Gpu->m_Desc.Height, static_cast<int>(s_Gpu->m_Desc.Format),
                      s_How.c_str(), s_Have ? "vanilla pixels from cache" : "vanilla pixels read from the GPU");
        SetLutStatus(3, s_Buf);
        return true;
    }

    SetLutStatus(1, s_FoundButNotLoaded ? "The Instinct LUT exists but is not loaded yet." : "The Instinct LUT is not part of this scene (yet).");
    return false;
}

// Puts the vanilla LUT back and lets go of the texture.
void DramaticAimingShift::RestoreTint() {
    if (m_Lut && m_LutApplied && m_HasPlayer.load() && LutStillValid()) {
        std::string s_Err;
        LutWrite(*m_Lut, m_LutBarrier, m_Lut->m_Base, s_Err);
    }
    ReleaseLut();
}

// Runs every frame from the game loop (also outside missions).
void DramaticAimingShift::UpdateTint(float p_Dt, bool /*p_Active*/) {
    if (!m_HasPlayer.load()) {
        // Out of the mission: the texture is about to go away. Drop our references without touching it.
        if (m_NoPlayerTime > 0.25f && m_Lut) ReleaseLut();
        return;
    }

    if (m_LutReread.exchange(false)) {
        RestoreTint(); // vanilla pixels back first, so the new readback is the real vanilla LUT
        std::error_code s_Ec;
        const fs::path s_Cache = LutCachePath();
        if (!s_Cache.empty()) fs::remove(s_Cache, s_Ec);
        m_LutPoll = 0.0f;
    }

    if (!m_TintEnabled && !m_Lut) return; // nothing to do and nothing to undo

    m_LutPoll -= p_Dt;
    m_LutCooldown -= p_Dt;

    if (m_Lut && !LutStillValid()) ReleaseLut();

    if (!m_Lut) {
        if (!m_TintEnabled || m_LutPoll > 0.0f) return;
        m_LutPoll = 2.0f; // searching walks the whole resource list: only every couple of seconds
        if (!EnsureLut()) return;
    }

    // What should the texture contain right now?
    uint32_t s_Sig = 2166136261u;
    auto s_Mix = [&](uint32_t p_Value) { s_Sig = (s_Sig ^ p_Value) * 16777619u; };
    s_Mix(m_TintEnabled ? 1u : 0u);
    if (m_TintEnabled) {
        for (int i = 0; i < 3; ++i) s_Mix(static_cast<uint32_t>(std::lround(std::clamp(m_InstinctTint[i], 0.0f, 1.0f) * 255.0f)));
        s_Mix(static_cast<uint32_t>(std::lround(std::clamp(m_TintStrength, 0.0f, 1.0f) * 255.0f)));
        s_Mix(m_TintKeepBrightness ? 1u : 0u);
        s_Mix(static_cast<uint32_t>(std::lround(m_TintBrightness * 100.0f)));
    }

    if (m_LutApplied && s_Sig == m_LutAppliedSig) return; // already up to date
    if (!m_LutApplied && !m_TintEnabled) {                // vanilla is already in place: let go of the texture
        ReleaseLut();
        return;
    }
    if (s_Sig == m_LutFailSig.load()) return;             // this exact setting failed already: wait for a change

    if (m_LutCooldown > 0.0f) return; // while dragging the color picker, write at most ~10 times per second
    m_LutCooldown = 0.1f;

    std::string s_Err;
    if (m_TintEnabled) {
        const std::vector<uint8_t> s_Image = LutRecolor(m_Lut->m_Base, m_InstinctTint, std::clamp(m_TintStrength, 0.0f, 1.0f),
                                                        m_TintKeepBrightness, m_TintBrightness);
        if (LutWrite(*m_Lut, m_LutBarrier, s_Image, s_Err)) {
            m_LutApplied = true;
            m_LutAppliedSig = s_Sig;
            ++m_LutWrites;
            SetLutStatus(4, "Instinct color is active (the game's grey Instinct LUT has been replaced).");
        } else {
            SetLutStatus(2, "Could not write the Instinct LUT: " + s_Err + ". Try the other 'GPU state' option under Advanced.");
            m_LutFailSig.store(s_Sig);
            m_LutApplied = false;
        }
    } else {
        if (LutWrite(*m_Lut, m_LutBarrier, m_Lut->m_Base, s_Err)) {
            m_LutApplied = false;
            m_LutAppliedSig = 0;
            SetLutStatus(3, "Instinct color is off (vanilla LUT restored).");
        }
    }
}


// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

DramaticAimingShift::DramaticAimingShift() {
    m_PinEnterAim = Crc32("EnterAimAt");
    m_PinExitAim  = Crc32("ExitAimAt");
}

DramaticAimingShift::~DramaticAimingShift() {
    EndEffect();
    if (m_TapPending) { SendKey(m_TapVk, true); m_TapPending = false; }
    RestoreTint();
    CloseSound();
    Hooks::SignalOutputPin->RemoveDetoursWithContext(this);
    Hooks::GetApplicationOptionBool->RemoveDetoursWithContext(this);

    const ZMemberDelegate<DramaticAimingShift, void(const SGameUpdateEvent&)> s_Delegate(this, &DramaticAimingShift::OnFrameUpdate);
    Globals::GameLoopManager->UnregisterFrameUpdate(s_Delegate, 1, EUpdateMode::eUpdatePlayMode);
}

void DramaticAimingShift::Init() {
    LoadSettings();
    m_SoundDir = ModuleDir() + L"DramaticAimingSounds\\";
    std::error_code s_Ec;
    fs::create_directories(fs::path(m_SoundDir), s_Ec);
    ScanSounds();
    Hooks::SignalOutputPin->AddDetour(this, &DramaticAimingShift::OnPinOutput);
    Hooks::GetApplicationOptionBool->AddDetour(this, &DramaticAimingShift::GetOption);
}

void DramaticAimingShift::OnEngineInitialized() {
    const ZMemberDelegate<DramaticAimingShift, void(const SGameUpdateEvent&)> s_Delegate(this, &DramaticAimingShift::OnFrameUpdate);
    Globals::GameLoopManager->RegisterFrameUpdate(s_Delegate, 1, EUpdateMode::eUpdatePlayMode);
    m_LastTick = std::chrono::steady_clock::now();
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

void DramaticAimingShift::LoadSettings() {
    m_Enabled = GetSettingBool(kSection, "enabled", m_Enabled);
    m_RequireShift = GetSettingBool(kSection, "require_shift", m_RequireShift);
    m_TimeScale = std::clamp(static_cast<float>(GetSettingDouble(kSection, "time_scale", m_TimeScale)), 0.05f, 1.0f);
    m_SoundEnabled = GetSettingBool(kSection, "sound_enabled", m_SoundEnabled);
    m_SoundVolume = std::clamp(static_cast<float>(GetSettingDouble(kSection, "sound_volume", m_SoundVolume)), 0.0f, 1.0f);
    m_UseInstinct = GetSettingBool(kSection, "use_instinct_state", m_UseInstinct);
    m_InstinctKeyIdx = std::clamp(static_cast<int>(GetSettingInt(kSection, "instinct_key_index", m_InstinctKeyIdx)), 0, kKeyCount - 1);
    m_InstinctToggle = GetSettingBool(kSection, "instinct_toggle", m_InstinctToggle);
    m_TintEnabled = GetSettingBool(kSection, "tint_enabled", m_TintEnabled);
    m_InstinctTint[0] = std::clamp(static_cast<float>(GetSettingDouble(kSection, "tint_r", m_InstinctTint[0])), 0.0f, 1.0f);
    m_InstinctTint[1] = std::clamp(static_cast<float>(GetSettingDouble(kSection, "tint_g", m_InstinctTint[1])), 0.0f, 1.0f);
    m_InstinctTint[2] = std::clamp(static_cast<float>(GetSettingDouble(kSection, "tint_b", m_InstinctTint[2])), 0.0f, 1.0f);
    m_TintStrength = std::clamp(static_cast<float>(GetSettingDouble(kSection, "tint_strength", m_TintStrength)), 0.0f, 1.0f);
    m_TintKeepBrightness = GetSettingBool(kSection, "tint_keep_brightness", m_TintKeepBrightness);
    m_TintBrightness = std::clamp(static_cast<float>(GetSettingDouble(kSection, "tint_brightness", m_TintBrightness)), 0.25f, 3.0f);
    m_LutBarrier = std::clamp(static_cast<int>(GetSettingInt(kSection, "lut_barrier", m_LutBarrier)), 0, 1);
}

void DramaticAimingShift::SaveSettings() {
    SetSettingBool(kSection, "enabled", m_Enabled);
    SetSettingBool(kSection, "require_shift", m_RequireShift);
    SetSettingDouble(kSection, "time_scale", m_TimeScale);
    SetSettingBool(kSection, "sound_enabled", m_SoundEnabled);
    SetSettingDouble(kSection, "sound_volume", m_SoundVolume);
    SetSettingBool(kSection, "use_instinct_state", m_UseInstinct);
    SetSettingInt(kSection, "instinct_key_index", m_InstinctKeyIdx);
    SetSettingBool(kSection, "instinct_toggle", m_InstinctToggle);
    SetSettingBool(kSection, "tint_enabled", m_TintEnabled);
    SetSettingDouble(kSection, "tint_r", m_InstinctTint[0]);
    SetSettingDouble(kSection, "tint_g", m_InstinctTint[1]);
    SetSettingDouble(kSection, "tint_b", m_InstinctTint[2]);
    SetSettingDouble(kSection, "tint_strength", m_TintStrength);
    SetSettingBool(kSection, "tint_keep_brightness", m_TintKeepBrightness);
    SetSettingDouble(kSection, "tint_brightness", m_TintBrightness);
    SetSettingInt(kSection, "lut_barrier", m_LutBarrier);

    // chosen sound file name
    const std::wstring s_Dir = ConfigDir();
    if (!s_Dir.empty() && m_SoundIndex >= 0 && m_SoundIndex < static_cast<int>(m_SoundFiles.size())) {
        std::error_code s_Ec;
        fs::create_directories(fs::path(s_Dir), s_Ec);
        std::ofstream s_Out(fs::path(s_Dir) / L"sound.txt", std::ios::trunc);
        if (s_Out) s_Out << m_SoundFiles[m_SoundIndex];
    }
}

// ---------------------------------------------------------------------------
// Aim detection (same signals the original mod used)
// ---------------------------------------------------------------------------

DEFINE_PLUGIN_DETOUR(DramaticAimingShift, bool, OnPinOutput, ZEntityRef p_Entity, uint32_t p_PinId, const ZObjectRef& p_Data) {
    if (p_PinId == m_PinEnterAim) {
        m_AimPin = true;
        m_ExitIgnored = false;
    } else if (p_PinId == m_PinExitAim) {
        // Shift (a movement key) can make the game drop its aim state; if Shift and right-click are
        // both held, that is not the player stopping.
        if (m_RequireShift && IsShiftHeld() && (GetAsyncKeyState(VK_RBUTTON) & 0x8000)) {
            m_ExitIgnored = true;
        } else {
            m_AimPin = false;
            m_ExitIgnored = false;
        }
    }
    return HookResult<bool>(HookAction::Continue());
}


// ---------------------------------------------------------------------------
// Option probe: record option names that look related to Instinct outlines, and answer "false" to
// the outline/glow/highlight/path ones while the effect is active.
// ---------------------------------------------------------------------------
DEFINE_PLUGIN_DETOUR(DramaticAimingShift, bool, GetOption, const ZString& p_OptionName, bool p_Default) {
    const char* s_Raw = p_OptionName.c_str();
    if (!s_Raw)
        return HookResult<bool>(HookAction::Continue());

    std::string s_Lower = s_Raw;
    for (auto& c : s_Lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    const bool s_Instinct = s_Lower.find("instinct") != std::string::npos;
    const bool s_Outline = s_Lower.find("glow") != std::string::npos || s_Lower.find("outline") != std::string::npos ||
                           s_Lower.find("highlight") != std::string::npos || s_Lower.find("path") != std::string::npos;

    if (s_Instinct || s_Outline || s_Lower.find("hint") != std::string::npos) {
        std::lock_guard<std::mutex> s_Lock(m_OptMutex);
        if (m_SeenOptions.size() < 200)
            m_SeenOptions.insert(s_Raw);
    }

    if (m_SuppressOutlines.load() && s_Instinct && s_Outline)
        return HookResult<bool>(HookAction::Return(), false);

    return HookResult<bool>(HookAction::Continue());
}

bool DramaticAimingShift::IsShiftHeld() const {
    return GameWindowFocused() && (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
}

// ---------------------------------------------------------------------------
// Per-frame logic
// ---------------------------------------------------------------------------

void DramaticAimingShift::OnFrameUpdate(const SGameUpdateEvent&) {
    const auto s_Now = std::chrono::steady_clock::now();
    const float s_Dt = std::min(0.1f, std::chrono::duration<float>(s_Now - m_LastTick).count());
    m_LastTick = s_Now;

    bool s_HasPlayer = false;
    if (auto s_LocalHitman = SDK()->GetLocalPlayer()) {
        s_HasPlayer = true;
    }
    m_HasPlayer = s_HasPlayer;
    m_NoPlayerTime = s_HasPlayer ? 0.0f : m_NoPlayerTime + s_Dt;

    if (m_ExitIgnored && !(GetAsyncKeyState(VK_RBUTTON) & 0x8000)) {
        m_AimPin = false;
        m_ExitIgnored = false;
    }

    const bool s_Want = s_HasPlayer && m_Enabled && m_AimPin && (!m_RequireShift || IsShiftHeld());

    if (s_Want && !m_Active)
        BeginEffect();
    else if (!s_Want && m_Active)
        EndEffect();

    if (m_Active) {
        m_ActiveTime += s_Dt;

        if (!m_TimeApplied && m_ActiveTime >= m_SlowMoDelay)
            m_TimeApplied = true;

        // The game can reset the multiplier by itself, so keep re-applying it while active.
        if (m_TimeApplied)
            SetTimeScale(m_TimeScale);
    }

    if (m_TapPending && s_Now >= m_TapReleaseAt) {
        SendKey(m_TapVk, true);
        m_TapPending = false;
    }

    UpdateSound(s_Dt);
    UpdateTint(s_Dt, m_Active);
}

void DramaticAimingShift::BeginEffect() {
    m_Active = true;
    m_ActiveTime = 0.f;
    m_TimeApplied = false;

    m_SndWanted = m_SoundEnabled;
    m_SuppressOutlines = true;

    // The game only blends its colour-grading LUT (the one we recolour) while its Instinct state is on.
    if (m_UseInstinct && m_TintEnabled) {
        const int s_Vk = kKeys[m_InstinctKeyIdx].Vk;
        if (!(GetAsyncKeyState(s_Vk) & 0x8000)) {
            SendKey(s_Vk, false);
            if (m_InstinctToggle) {
                // Toggle mode: a short tap switches Instinct on; we tap again when the effect ends.
                m_TapPending = true;
                m_TapVk = s_Vk;
                m_TapReleaseAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(60);
                m_InstinctToggledOn = true;
            } else {
                m_InstinctHeld = true;
            }
        }
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

    m_SndWanted = false;
    m_SuppressOutlines = false;

    if (m_InstinctHeld) {
        SendKey(kKeys[m_InstinctKeyIdx].Vk, true);
        m_InstinctHeld = false;
    }
    if (m_InstinctToggledOn) {
        const int s_Vk = kKeys[m_InstinctKeyIdx].Vk;
        if (m_TapPending) SendKey(m_TapVk, true); // finish the first tap before starting the second
        SendKey(s_Vk, false);
        m_TapPending = true;
        m_TapVk = s_Vk;
        m_TapReleaseAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(60);
        m_InstinctToggledOn = false;
    }
}

void DramaticAimingShift::SetTimeScale(float p_Scale) {
    if (Globals::GameTimeManager)
        Globals::GameTimeManager->m_fGameTimeMultiplier = p_Scale;
}

// ---------------------------------------------------------------------------
// Sound (Windows MCI: wav and mp3, looping, volume fades)
// ---------------------------------------------------------------------------

void DramaticAimingShift::ScanSounds() {
    m_SoundFiles.clear();
    std::error_code s_Ec;
    for (const auto& s_Entry : fs::directory_iterator(fs::path(m_SoundDir), s_Ec)) {
        if (!s_Entry.is_regular_file()) continue;
        std::wstring s_Ext = s_Entry.path().extension().wstring();
        for (auto& c : s_Ext) c = static_cast<wchar_t>(std::towlower(c));
        if (s_Ext == L".wav" || s_Ext == L".mp3")
            m_SoundFiles.push_back(WideToUtf8(s_Entry.path().filename().wstring()));
    }
    std::sort(m_SoundFiles.begin(), m_SoundFiles.end());

    // restore the saved choice; otherwise prefer something called "ave...", else the first file
    std::string s_Saved;
    const std::wstring s_Dir = ConfigDir();
    if (!s_Dir.empty()) {
        std::ifstream s_In(fs::path(s_Dir) / L"sound.txt");
        if (s_In) std::getline(s_In, s_Saved);
    }
    m_SoundIndex = 0;
    bool s_Found = false;
    for (size_t i = 0; i < m_SoundFiles.size(); ++i) {
        if (!s_Saved.empty() && m_SoundFiles[i] == s_Saved) { m_SoundIndex = static_cast<int>(i); s_Found = true; break; }
    }
    if (!s_Found) {
        for (size_t i = 0; i < m_SoundFiles.size(); ++i) {
            std::string s_Lower = m_SoundFiles[i];
            for (auto& c : s_Lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (s_Lower.find("ave") != std::string::npos) { m_SoundIndex = static_cast<int>(i); break; }
        }
    }
    m_SoundStatus = m_SoundFiles.empty() ? "No .wav/.mp3 files in the DramaticAimingSounds folder." : "";
}

std::wstring DramaticAimingShift::CurrentSoundPath() const {
    if (m_SoundIndex < 0 || m_SoundIndex >= static_cast<int>(m_SoundFiles.size())) return L"";
    return m_SoundDir + Utf8ToWide(m_SoundFiles[m_SoundIndex]);
}

void DramaticAimingShift::ApplySoundVolume() {
    const int s_Vol = std::clamp(static_cast<int>(m_SndGain * m_SoundVolume * 1000.0f), 0, 1000);
    Mci(L"setaudio dasnd volume to " + std::to_wstring(s_Vol));
}

void DramaticAimingShift::StartSound() {
    const std::wstring s_Path = CurrentSoundPath();
    if (s_Path.empty() || !fs::exists(fs::path(s_Path))) {
        m_SoundStatus = "Sound file not found.";
        return;
    }
    Mci(L"close dasnd");
    const std::wstring s_Cmd = L"open \"" + s_Path + L"\" alias dasnd";
    if (mciSendStringW(s_Cmd.c_str(), nullptr, 0, nullptr) != 0) {
        m_SoundStatus = "Couldn't open that file (use .wav or .mp3).";
        return;
    }
    m_SoundStatus.clear();
    m_SndOpen = true;
    m_SndGain = 0.f;
    ApplySoundVolume();
    Mci(L"play dasnd from 0 repeat");
}

void DramaticAimingShift::CloseSound() {
    if (m_SndOpen) {
        Mci(L"stop dasnd");
        Mci(L"close dasnd");
        m_SndOpen = false;
    }
    m_SndGain = 0.f;
}

void DramaticAimingShift::StopSound() {
    // fading out happens in UpdateSound
}

void DramaticAimingShift::UpdateSound(float p_Dt) {
    const bool s_Wanted = m_SoundEnabled && m_SndWanted;

    if (s_Wanted && !m_SndOpen)
        StartSound();

    if (!m_SndOpen)
        return;

    const float s_Step = p_Dt / std::max(0.001f, m_SoundFade);
    m_SndGain = s_Wanted ? std::min(1.f, m_SndGain + s_Step) : std::max(0.f, m_SndGain - s_Step);
    ApplySoundVolume();

    if (!s_Wanted && m_SndGain <= 0.f)
        CloseSound();
}

// ---------------------------------------------------------------------------
// Menu
// ---------------------------------------------------------------------------

void DramaticAimingShift::OnDrawMenu() {
    if (!ImGui::BeginMenu("Dramatic Aiming (Shift)"))
        return;

    bool s_Changed = false;
    s_Changed |= ImGui::Checkbox("Enabled", &m_Enabled);
    s_Changed |= ImGui::Checkbox("Require Shift while aiming", &m_RequireShift);
    s_Changed |= ImGui::SliderFloat("Time scale", &m_TimeScale, 0.05f, 1.f, "%.2fx");

    ImGui::Separator();
    s_Changed |= ImGui::Checkbox("Play custom sound", &m_SoundEnabled);
    s_Changed |= ImGui::SliderFloat("Sound volume", &m_SoundVolume, 0.f, 1.f);
    if (!m_SoundFiles.empty()) {
        const char* s_Cur = m_SoundIndex >= 0 && m_SoundIndex < static_cast<int>(m_SoundFiles.size())
                                ? m_SoundFiles[m_SoundIndex].c_str() : "";
        if (ImGui::BeginCombo("Sound file", s_Cur)) {
            for (int i = 0; i < static_cast<int>(m_SoundFiles.size()); ++i) {
                if (ImGui::Selectable(m_SoundFiles[i].c_str(), i == m_SoundIndex)) {
                    m_SoundIndex = i;
                    s_Changed = true;
                    CloseSound();
                }
            }
            ImGui::EndCombo();
        }
    }
    if (ImGui::Button("Rescan sounds")) ScanSounds();
    ImGui::SameLine();
    if (ImGui::Button("Open sound folder")) ShellExecuteW(nullptr, L"open", m_SoundDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (!m_SoundStatus.empty()) ImGui::TextWrapped("%s", m_SoundStatus.c_str());

    ImGui::Separator();
    s_Changed |= ImGui::Checkbox("Grey overlay", &m_TintEnabled);
    s_Changed |= ImGui::Checkbox("Use the game's Instinct state to show it", &m_UseInstinct);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The game only draws its colour-grading overlay while Instinct is on.\nThis presses your Instinct key while the effect is active.");
    if (m_UseInstinct) {
        s_Changed |= ImGui::Checkbox("My Instinct is set to Toggle (not Hold) in the game settings", &m_InstinctToggle);
        if (ImGui::BeginCombo("Instinct key", kKeys[m_InstinctKeyIdx].Name)) {
            for (int i = 0; i < kKeyCount; ++i)
                if (ImGui::Selectable(kKeys[i].Name, i == m_InstinctKeyIdx)) { m_InstinctKeyIdx = i; s_Changed = true; }
            ImGui::EndCombo();
        }
    }
    s_Changed |= ImGui::ColorEdit3("Overlay color", m_InstinctTint);
    s_Changed |= ImGui::SliderFloat("Overlay strength", &m_TintStrength, 0.f, 1.f);
    s_Changed |= ImGui::Checkbox("Keep brightness", &m_TintKeepBrightness);
    s_Changed |= ImGui::SliderFloat("Overlay brightness", &m_TintBrightness, 0.25f, 3.f);
    {
        std::lock_guard<std::mutex> s_Lock(m_LutMutex);
        if (!m_LutStatus.empty()) ImGui::TextWrapped("%s", m_LutStatus.c_str());
    }

    ImGui::Separator();
    if (ImGui::TreeNode("Option names seen (outline probe)")) {
        std::lock_guard<std::mutex> s_Lock(m_OptMutex);
        if (m_SeenOptions.empty())
            ImGui::TextDisabled("(none yet - aim with Shift once)");
        for (const auto& s_Name : m_SeenOptions)
            ImGui::TextUnformatted(s_Name.c_str());
        ImGui::TreePop();
    }

    ImGui::Separator();
    ImGui::Text("aimPin=%d  shift=%d  active=%d  gameScale=%.2f", (int)m_AimPin.load(), (int)IsShiftHeld(), (int)m_Active,
                Globals::GameTimeManager ? Globals::GameTimeManager->m_fGameTimeMultiplier : -1.f);

    if (s_Changed) SaveSettings();
    ImGui::EndMenu();
}

DEFINE_ZHM_PLUGIN(DramaticAimingShift)
