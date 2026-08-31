// NRProcessor.h - DLSS 5 Neural Rendering post-processor for MPC Video Renderer.
//
// Runs the NVIDIA DLSS 5 NR model (NGX feature 18) on the D3D11-rendered frame.
// The renderer's D3D11 device keeps ownership of the frame; this module creates
// a matching D3D12 device (same adapter LUID) and shares the frame through
// D3D11<->D3D12 shared textures, ordered by a single shared fence.
//
// The NGX interface is declared inline (no NVIDIA SDK headers required); the
// NGX core/snippet/shims are loaded dynamically at runtime and are NOT
// redistributable (see the project README).

#pragma once

// DLSS 5 NR needs D3D11.4 shared fences (Windows 10 1703+). The renderer targets
// Windows 7 (WINVER/_WIN32_WINNT = 0x0601), so NTDDI_VERSION is too low for
// d3d11_4.h to declare ID3D11Fence / ID3D11Device5 / ID3D11DeviceContext4.
// Raise it for these includes only, then restore.
#pragma push_macro("NTDDI_VERSION")
#undef NTDDI_VERSION
#define NTDDI_VERSION 0x0A000007 // NTDDI_WIN10_20H1 (>= RS2)

#include <d3d11.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_5.h>
#include <string>

#pragma pop_macro("NTDDI_VERSION")


struct NVSDK_NGX_Parameter; // defined in NRProcessor.cpp
struct NVSDK_NGX_Handle;    // defined in NRProcessor.cpp

class CNRProcessor
{
public:
    CNRProcessor();
    ~CNRProcessor();

    // Create the D3D12 device (same adapter as pDevice11) and initialise NGX.
    bool Init(ID3D11Device* pDevice11);
    void Shutdown();

    // Neural-render the frame (a B8G8R8A8_UNORM D3D11 texture) in place.
    // Returns false if NR is unavailable/disabled; the frame is left untouched.
    bool Process(ID3D11DeviceContext* pCtx11, ID3D11Texture2D* pFrame);

    bool IsEnabled() const { return m_bEnabled && m_bInited; }
    bool IsInited() const { return m_bInited; }
    void SetEnabled(bool b) { m_bEnabled = b; }

    void SetStyle(const std::string& s)  { m_style = s;  m_bDirty = true; }
    void SetPreset(int v)                { m_preset = v; m_bDirty = true; }
    void SetIntensity(int v)             { m_intensity = v; m_bDirty = true; }
    void SetTone(int v)                  { m_tone = v; m_bDirty = true; }
    void SetStructure(int v)             { m_structure = v; m_bDirty = true; }
    void SetSkin(int v)                  { m_skin = v; m_bDirty = true; }
    void SetMask(int v)                  { m_mask = v; m_bDirty = true; }

private:
    bool CreateD3D12(ID3D11Device* pDevice11);
    bool SetupCompute();
    bool EnsureResources(UINT w, UINT h);
    bool EnsureInterop(ID3D11Device* dev11, ID3D11DeviceContext* pCtx11);
    bool SpawnWorker(UINT w, UINT h, HANDLE hIn, HANDLE hOut);

    // D3D12
    CComPtr<ID3D12Device>             m_dev12;
    CComPtr<ID3D12CommandQueue>       m_queue12;
    CComPtr<ID3D12CommandAllocator>   m_alloc12;
    CComPtr<ID3D12GraphicsCommandList> m_list12;
    CComPtr<ID3D12Fence>              m_fence12;
    HANDLE                           m_fenceEvent = nullptr;
    UINT64                           m_fenceValue = 0;

    // D3D11 interop (shared fence + context4)
    CComPtr<ID3D11Fence>              m_fence11;
    CComPtr<ID3D11DeviceContext4>     m_ctx4;
    CComPtr<ID3D11Query>              m_query;   // event query: CPU-side D3D11->D3D12 sync

    // shared textures (D3D11 side owns, D3D12 opens)
    CComPtr<ID3D11Texture2D>          m_in11;    // matches the backbuffer format
    CComPtr<ID3D11Texture2D>          m_out11;   // R8G8B8A8 (UAV-capable in D3D12)
    CComPtr<ID3D12Resource>           m_in12;
    CComPtr<ID3D12Resource>           m_out12;
    DXGI_FORMAT                       m_frameFormat = DXGI_FORMAT_B8G8R8A8_UNORM;

    // D3D12 NR working set (RGBA16F), owned by D3D11 shared textures for the worker
    CComPtr<ID3D11Texture2D>          m_nrIn11;   // shared with the NR worker (input)
    CComPtr<ID3D11Texture2D>          m_nrOut11;  // shared with the NR worker (output)
    CComPtr<ID3D12Resource>           m_nrIn;     // opened in D3D12
    CComPtr<ID3D12Resource>           m_nrOut;    // opened in D3D12

    // compute pipeline
    CComPtr<ID3D12RootSignature>      m_rs;
    CComPtr<ID3D12PipelineState>      m_pso1;   // B8G8R8A8 -> RGBA16F
    CComPtr<ID3D12PipelineState>      m_pso2;   // RGBA16F -> R8G8B8A8
    CComPtr<ID3D12DescriptorHeap>     m_heap;

    // NR worker process (NGX runs in a separate process, which is the only
    // environment where nvngx_dlssnr.dll's Init_Ext succeeds on this machine)
    HANDLE           m_hFrameReady = nullptr;
    HANDLE           m_hFrameDone  = nullptr;
    HANDLE           m_hShutdown   = nullptr;
    PROCESS_INFORMATION m_piWorker = {};
    bool             m_bWorkerRunning = false;

    // D3D11 copy-back (output R8G8B8A8 -> backbuffer B8G8R8A8)
    CComPtr<ID3D11VertexShader>       m_vsCopy;
    CComPtr<ID3D11PixelShader>        m_psCopy;
    CComPtr<ID3D11InputLayout>        m_ilCopy;
    CComPtr<ID3D11SamplerState>       m_sampler;
    CComPtr<ID3D11ShaderResourceView> m_srvOut;

    // config
    bool        m_bEnabled = true;
    bool        m_bInited = false;
    bool        m_bDirty = false;
    bool        m_bFirstFrame = true;
    int         m_intensity = 2, m_preset = 3, m_tone = 1, m_structure = 1, m_skin = -1, m_mask = 0;
    std::string m_style = "cinematic";
    UINT        m_w = 0, m_h = 0;
    UINT        m_dominantW = 0, m_dominantH = 0;
    uint64_t    m_dominantArea = 0;
    UINT        m_dominantCount = 0;
};
