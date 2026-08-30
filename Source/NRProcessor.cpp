// NRProcessor.cpp - DLSS 5 Neural Rendering post-processor for MPC Video Renderer.
//
// See NRProcessor.h for the design. The NGX interface is declared inline here
// (no NVIDIA SDK headers); NGX core/snippet/shim are loaded dynamically.
// The NVIDIA DLLs are NOT redistributable - see the project README.

#include "stdafx.h"
#include "NRProcessor.h"
#include <d3dcompiler.h>
#include <cstdio>
#include <cstdarg>
#include <vector>

#ifndef NVSDK_CONV
#define NVSDK_CONV __cdecl
#endif

// ---------------------------------------------------------------------------
// inline NGX interface (mirrors NVIDIA's layout, no SDK headers needed)
// ---------------------------------------------------------------------------
typedef int NVSDK_NGX_Result;
static const NVSDK_NGX_Result NGX_SUCCESS = 1;
static const int NR_FEATURE_ID = 18;

struct NVSDK_NGX_Handle { unsigned int Id; };

struct NVSDK_NGX_Parameter
{
    virtual void Set(const char* InName, unsigned long long InValue) = 0;
    virtual void Set(const char* InName, float InValue) = 0;
    virtual void Set(const char* InName, double InValue) = 0;
    virtual void Set(const char* InName, unsigned int InValue) = 0;
    virtual void Set(const char* InName, int InValue) = 0;
    virtual void Set(const char* InName, ID3D11Resource* InValue) = 0;
    virtual void Set(const char* InName, ID3D12Resource* InValue) = 0;
    virtual void Set(const char* InName, void* InValue) = 0;
    virtual NVSDK_NGX_Result Get(const char* InName, unsigned long long* OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char* InName, float* OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char* InName, double* OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char* InName, unsigned int* OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char* InName, int* OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char* InName, ID3D11Resource** OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char* InName, ID3D12Resource** OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char* InName, void** OutValue) const = 0;
    virtual void Reset() = 0;
};

typedef struct NVSDK_NGX_PathListInfo
{
    wchar_t const* const* Path;
    unsigned int Length;
} NVSDK_NGX_PathListInfo;

typedef enum NVSDK_NGX_Logging_Level
{
    NVSDK_NGX_LOGGING_LEVEL_OFF = 0,
    NVSDK_NGX_LOGGING_LEVEL_ON,
    NVSDK_NGX_LOGGING_LEVEL_VERBOSE,
} NVSDK_NGX_Logging_Level;

typedef void(NVSDK_CONV* NVSDK_NGX_AppLogCallback)(const char*, NVSDK_NGX_Logging_Level, int);

typedef struct NVSDK_NGX_LoggingInfo
{
    NVSDK_NGX_Logging_Level LoggingLevel;
    NVSDK_NGX_AppLogCallback Callback;
    void* UserData;
    bool DisableOtherLoggingSinks;
} NVSDK_NGX_LoggingInfo;

typedef struct NVSDK_NGX_FeatureCommonInfo_Internal NVSDK_NGX_FeatureCommonInfo_Internal;

typedef struct NVSDK_NGX_FeatureCommonInfo
{
    NVSDK_NGX_PathListInfo PathListInfo;
    NVSDK_NGX_FeatureCommonInfo_Internal* InternalData;
    NVSDK_NGX_LoggingInfo LoggingInfo;
} NVSDK_NGX_FeatureCommonInfo;

// function pointers
typedef NVSDK_NGX_Result (*PFN_Init_Ext)(unsigned long long, const wchar_t*, ID3D12Device*, int, const void*);
typedef NVSDK_NGX_Result (*PFN_Init_ProjectID)(const char*, int, const char*, const wchar_t*, ID3D12Device*, int, const void*);
typedef NVSDK_NGX_Result (*PFN_ShimInit)(void*, unsigned long long, const wchar_t*, ID3D12Device*, int, const void*);
typedef NVSDK_NGX_Result (*PFN_ShimCreate)(void*, ID3D12GraphicsCommandList*, int, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
typedef NVSDK_NGX_Result (*PFN_ShimEvaluate)(void*, ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*, void*);
typedef NVSDK_NGX_Result (*PFN_ShimRelease)(void*, NVSDK_NGX_Handle*);
typedef NVSDK_NGX_Result (*PFN_AllocateParameters)(NVSDK_NGX_Parameter**);
typedef NVSDK_NGX_Result (*PFN_D3D12CreateFeature)(ID3D12GraphicsCommandList*, int, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
typedef NVSDK_NGX_Result (*PFN_D3D12EvaluateFeature)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*, void*);
typedef NVSDK_NGX_Result (*PFN_D3D12ReleaseFeature)(NVSDK_NGX_Handle*);
typedef NVSDK_NGX_Result (*PFN_Shutdown)(void);

// NGX entry points (module globals)
static PFN_Init_ProjectID       s_initProjectID;
static PFN_AllocateParameters   s_alloc;
static PFN_D3D12CreateFeature   s_nrCreate;
static PFN_D3D12EvaluateFeature s_nrEval;
static PFN_D3D12ReleaseFeature  s_nrRelease;
static PFN_Init_Ext             s_directInit;
static PFN_ShimInit             s_shimInit;
static PFN_ShimCreate           s_shimCreate;
static PFN_ShimEvaluate         s_shimEval;
static PFN_ShimRelease          s_shimRelease;

// ---------------------------------------------------------------------------
// small logging helper (goes to MPC-BE's debug log if attached)
// ---------------------------------------------------------------------------
static void NRLog(const char* fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf_s(buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");
}

static D3D12_RESOURCE_BARRIER Trans(ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    return b;
}

// ---------------------------------------------------------------------------
// CNRProcessor
// ---------------------------------------------------------------------------
CNRProcessor::CNRProcessor() = default;

CNRProcessor::~CNRProcessor()
{
    Shutdown();
}

bool CNRProcessor::Init(ID3D11Device* pDevice11)
{
    if (m_bInited) return true;
    if (!CreateD3D12(pDevice11)) return false;
    if (!SetupCompute()) return false;
    m_bInited = true;
    return true;
}

void CNRProcessor::Shutdown()
{
    if (m_feature && s_nrRelease)
    {
        if (s_shimRelease) s_shimRelease((void*)s_nrRelease, m_feature);
        else s_nrRelease(m_feature);
        m_feature = nullptr;
    }
    if (m_fenceEvent) { CloseHandle(m_fenceEvent); m_fenceEvent = nullptr; }
    m_params = nullptr;
    m_list12.Release();
    m_alloc12.Release();
    m_queue12.Release();
    m_dev12.Release();
    m_in11.Release(); m_out11.Release();
    m_in12.Release(); m_out12.Release();
    m_nrIn.Release(); m_nrOut.Release();
    m_rs.Release(); m_pso1.Release(); m_pso2.Release(); m_heap.Release();
    m_fence11.Release(); m_fence12.Release(); m_ctx4.Release();
    m_vsCopy.Release(); m_psCopy.Release(); m_ilCopy.Release(); m_sampler.Release(); m_srvOut.Release();
    m_w = m_h = 0;
    m_bInited = false;
}

bool CNRProcessor::CreateD3D12(ID3D11Device* pDevice11)
{
    // find the DXGI adapter behind the D3D11 device
    CComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(pDevice11->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) { NRLog("[NR] no IDXGIDevice"); return false; }
    CComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgiDevice->GetAdapter(&adapter))) { NRLog("[NR] GetAdapter failed"); return false; }

    if (FAILED(D3D12CreateDevice(adapter.p, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_dev12))))
        { NRLog("[NR] D3D12CreateDevice failed"); return false; }

    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(m_dev12->CreateCommandQueue(&qd, IID_PPV_ARGS(&m_queue12)))) return false;
    if (FAILED(m_dev12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_alloc12)))) return false;
    if (FAILED(m_dev12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_alloc12.p, nullptr, IID_PPV_ARGS(&m_list12)))) return false;
    if (FAILED(m_dev12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&m_fence12)))) return false;
    m_list12->Close();

    // share the fence to D3D11
    HANDLE fh = nullptr;
    if (SUCCEEDED(m_dev12->CreateSharedHandle(m_fence12.p, nullptr, GENERIC_ALL, nullptr, &fh)))
    {
        CComPtr<ID3D11Device5> dev5;
        if (SUCCEEDED(pDevice11->QueryInterface(IID_PPV_ARGS(&dev5))))
            dev5->OpenSharedFence(fh, IID_PPV_ARGS(&m_fence11));
    }
    if (fh) CloseHandle(fh);
    if (!m_fence11) { NRLog("[NR] shared fence failed (need Win10 1703+ / D3D11.3)"); return false; }

    m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    NRLog("[NR] D3D12 device ready");
    return true;
}

bool CNRProcessor::SetupCompute()
{
    // root signature: SRV t0 + UAV u0
    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 1; ranges[0].BaseShaderRegister = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1; ranges[1].BaseShaderRegister = 0;
    D3D12_ROOT_PARAMETER rp[2] = {};
    for (int i = 0; i < 2; ++i)
    {
        rp[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[i].DescriptorTable.NumDescriptorRanges = 1;
        rp[i].DescriptorTable.pDescriptorRanges = &ranges[i];
    }
    D3D12_ROOT_SIGNATURE_DESC rsd = {};
    rsd.NumParameters = 2; rsd.pParameters = rp;
    CComPtr<ID3DBlob> sig, err;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)))
        { NRLog("[NR] root sig serialize failed"); return false; }
    if (FAILED(m_dev12->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&m_rs))))
        return false;

    const char* cs1 =
        "Texture2D<float4> src : register(t0);\n"
        "RWTexture2D<float4> dst : register(u0);\n"
        "[numthreads(16,16,1)]\n"
        "void CSMain(uint3 id : SV_DispatchThreadID) { dst[id.xy] = float4(src[id.xy].rgb, 1.0f); }\n";
    const char* cs2 =
        "Texture2D<float4> src : register(t0);\n"
        "RWTexture2D<unorm float4> dst : register(u0);\n"
        "[numthreads(16,16,1)]\n"
        "void CSMain(uint3 id : SV_DispatchThreadID) { float4 c = src[id.xy]; dst[id.xy] = float4(c.rgb, 1.0f); }\n";

    D3D12_COMPUTE_PIPELINE_STATE_DESC ps = {};
    ps.pRootSignature = m_rs.p;
    CComPtr<ID3DBlob> b1, e1, b2, e2;
    if (FAILED(D3DCompile(cs1, strlen(cs1), "cs1", nullptr, nullptr, "CSMain", "cs_5_0", 0, 0, &b1, &e1)))
        { NRLog("[NR] cs1 compile: %s", e1 ? (char*)e1->GetBufferPointer() : "?"); return false; }
    if (FAILED(D3DCompile(cs2, strlen(cs2), "cs2", nullptr, nullptr, "CSMain", "cs_5_0", 0, 0, &b2, &e2)))
        { NRLog("[NR] cs2 compile: %s", e2 ? (char*)e2->GetBufferPointer() : "?"); return false; }
    ps.CS = { b1->GetBufferPointer(), b1->GetBufferSize() };
    if (FAILED(m_dev12->CreateComputePipelineState(&ps, IID_PPV_ARGS(&m_pso1)))) return false;
    ps.CS = { b2->GetBufferPointer(), b2->GetBufferSize() };
    if (FAILED(m_dev12->CreateComputePipelineState(&ps, IID_PPV_ARGS(&m_pso2)))) return false;

    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 4; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(m_dev12->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_heap)))) return false;
    return true;
}

static bool MakeTex12(ID3D12Device* dev, UINT w, UINT h, DXGI_FORMAT fmt, D3D12_RESOURCE_STATES state,
                      D3D12_RESOURCE_FLAGS flags, CComPtr<ID3D12Resource>& out)
{
    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Alignment = 0;
    d.Width = w; d.Height = h; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.Format = fmt; d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    d.Flags = flags;
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    return SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&out)));
}

bool CNRProcessor::EnsureResources(UINT w, UINT h)
{
    if (w == m_w && h == m_h && m_nrIn) return true;

    // recreate NR feature if the size changed
    if (m_feature && (w != m_w || h != m_h))
    {
        if (s_shimRelease) s_shimRelease((void*)s_nrRelease, m_feature);
        else if (s_nrRelease) s_nrRelease(m_feature);
        m_feature = nullptr;
    }
    m_w = w; m_h = h;

    // NR working textures (RGBA16F)
    m_nrIn.Release(); m_nrOut.Release();
    if (!MakeTex12(m_dev12.p, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, m_nrIn)) return false;
    if (!MakeTex12(m_dev12.p, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, m_nrOut)) return false;

    // interop textures are (re)created lazily in EnsureInterop() on the next Process
    m_in11.Release(); m_out11.Release(); m_in12.Release(); m_out12.Release(); m_srvOut.Release();

    // descriptor views for the NR working textures
    UINT inc = m_dev12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE base = m_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

    // [0] SRV in12 (B8G8R8A8) - filled in EnsureInterop (texture created there)
    // [1] UAV nrIn (RGBA16F)
    uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    m_dev12->CreateUnorderedAccessView(m_nrIn.p, nullptr, &uav, { base.ptr + 1 * inc });
    // [2] SRV nrOut (RGBA16F)
    srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    m_dev12->CreateShaderResourceView(m_nrOut.p, &srv, { base.ptr + 2 * inc });
    // [3] UAV out12 (R8G8B8A8) - filled in EnsureInterop

    m_bFirstFrame = true;
    return true;
}

bool CNRProcessor::EnsureInterop(ID3D11Device* dev11, ID3D11DeviceContext* pCtx11)
{
    if (m_in11 && m_out11) return true;

    m_in11.Release(); m_out11.Release(); m_in12.Release(); m_out12.Release(); m_srvOut.Release();

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = m_w; td.Height = m_h; td.MipLevels = 1; td.ArraySize = 1;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;

    // input: B8G8R8A8 (matches the backbuffer, CopyResource in)
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.BindFlags = 0;
    if (FAILED(dev11->CreateTexture2D(&td, nullptr, &m_in11))) return false;

    // output: R8G8B8A8 (UAV-capable in D3D12) + SRV for the D3D11 copy-back
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev11->CreateTexture2D(&td, nullptr, &m_out11))) return false;
    dev11->CreateShaderResourceView(m_out11.p, nullptr, &m_srvOut);

    // share input -> D3D12
    HANDLE h = nullptr;
    CComPtr<IDXGIResource1> res1;
    if (SUCCEEDED(m_in11->QueryInterface(IID_PPV_ARGS(&res1))))
        res1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &h);
    if (h) { m_dev12->OpenSharedHandle(h, IID_PPV_ARGS(&m_in12)); CloseHandle(h); }

    // share output -> D3D12
    h = nullptr;
    CComPtr<IDXGIResource1> res2;
    if (SUCCEEDED(m_out11->QueryInterface(IID_PPV_ARGS(&res2))))
        res2->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &h);
    if (h) { m_dev12->OpenSharedHandle(h, IID_PPV_ARGS(&m_out12)); CloseHandle(h); }

    if (!m_in12 || !m_out12) { NRLog("[NR] interop share failed"); return false; }

    // finish the descriptor views that depend on the shared textures
    UINT inc = m_dev12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE base = m_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

    srv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    m_dev12->CreateShaderResourceView(m_in12.p, &srv, { base.ptr + 0 * inc });
    uav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    m_dev12->CreateUnorderedAccessView(m_out12.p, nullptr, &uav, { base.ptr + 3 * inc });

    // context4 (Signal/Wait) + D3D11 copy-back shader
    if (!m_ctx4)
        pCtx11->QueryInterface(IID_PPV_ARGS(&m_ctx4));

    const char* vs =
        "struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };\n"
        "O main(uint id : SV_VertexID) { O o; o.uv = float2((id << 1) & 2, id & 2); o.p = float4(o.uv * float2(2,-2) + float2(-1,1), 0, 1); return o; }\n";
    const char* ps =
        "Texture2D tex : register(t0); SamplerState s : register(s0);\n"
        "float4 main(float4 p : SV_Position, float2 uv : TEXCOORD0) : SV_Target { return tex.SampleLevel(s, uv, 0); }\n";
    CComPtr<ID3DBlob> vb, pb;
    if (SUCCEEDED(D3DCompile(vs, strlen(vs), "vs", nullptr, nullptr, "main", "vs_5_0", 0, 0, &vb, nullptr)))
        dev11->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, &m_vsCopy);
    if (SUCCEEDED(D3DCompile(ps, strlen(ps), "ps", nullptr, nullptr, "main", "ps_5_0", 0, 0, &pb, nullptr)))
        dev11->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, &m_psCopy);

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    dev11->CreateSamplerState(&sd, &m_sampler);

    NRLog("[NR] interop ready %ux%u", m_w, m_h);
    return true;
}

bool CNRProcessor::SetupNGX(UINT w, UINT h)
{
    if (m_feature) return true;

    // Resolve the NGX DLLs relative to the host module (mpc-be64.exe) so loading
    // does not depend on the process current directory.
    wchar_t exe_path[MAX_PATH] = L"";
    wchar_t exe_dir[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    wcscpy_s(exe_dir, exe_path);
    wchar_t* slash = wcsrchr(exe_dir, L'\\');
    if (slash) *(slash + 1) = 0;

    wchar_t path[MAX_PATH];

    HMODULE ngx = nullptr;
    swprintf_s(path, L"%ls_nvngx.dll", exe_dir);
    ngx = LoadLibraryW(path);
    if (!ngx)
    {
        WIN32_FIND_DATAW fd;
        wchar_t pat[MAX_PATH];
        swprintf_s(pat, L"%ls\\FileRepository\\nv_dispi.inf_*\\_nvngx.dll", L"C:\\Windows\\System32\\DriverStore");
        HANDLE hf = FindFirstFileW(pat, &fd);
        if (hf != INVALID_HANDLE_VALUE)
        {
            swprintf_s(path, L"C:\\Windows\\System32\\DriverStore\\FileRepository\\%ls", fd.cFileName);
            ngx = LoadLibraryW(path);
            FindClose(hf);
        }
    }
    if (!ngx) { NRLog("[NR] cannot load _nvngx.dll"); return false; }

    s_initProjectID = (PFN_Init_ProjectID)GetProcAddress(ngx, "NVSDK_NGX_D3D12_Init_ProjectID");
    s_alloc         = (PFN_AllocateParameters)GetProcAddress(ngx, "NVSDK_NGX_D3D12_AllocateParameters");

    swprintf_s(path, L"%lsnvngx_dlssnr.dll", exe_dir);
    HMODULE nr = LoadLibraryW(path);
    if (nr)
    {
        s_directInit = (PFN_Init_Ext)GetProcAddress(nr, "NVSDK_NGX_D3D12_Init_Ext");
        s_nrCreate   = (PFN_D3D12CreateFeature)GetProcAddress(nr, "NVSDK_NGX_D3D12_CreateFeature");
        s_nrEval     = (PFN_D3D12EvaluateFeature)GetProcAddress(nr, "NVSDK_NGX_D3D12_EvaluateFeature");
        s_nrRelease  = (PFN_D3D12ReleaseFeature)GetProcAddress(nr, "NVSDK_NGX_D3D12_ReleaseFeature");
    }

    swprintf_s(path, L"%lscaller\\nvngx.dll", exe_dir);
    HMODULE shim = LoadLibraryW(path);
    if (shim)
    {
        s_shimInit    = (PFN_ShimInit)GetProcAddress(shim, "DLSSNR_CallInit");
        s_shimCreate  = (PFN_ShimCreate)GetProcAddress(shim, "DLSSNR_CallCreate");
        s_shimEval    = (PFN_ShimEvaluate)GetProcAddress(shim, "DLSSNR_CallEvaluate");
        s_shimRelease = (PFN_ShimRelease)GetProcAddress(shim, "DLSSNR_CallRelease");
    }
    if (!s_initProjectID || !s_alloc || !s_nrCreate || !s_nrEval || !s_nrRelease)
        { NRLog("[NR] NGX entry points missing"); return false; }

    wchar_t data_path[MAX_PATH];
    wcscpy_s(data_path, exe_dir);
    const unsigned long long APP_ID = 141959980ULL;

    int inited = 0;
    for (int ver = 0x13; ver <= 0x20 && !inited; ++ver)
    {
        NVSDK_NGX_Result r = s_initProjectID("53f803cc-a12f-4d69-90d5-19b7599cad19", 0, "0.1",
                                              data_path, m_dev12.p, ver, nullptr);
        if (r == NGX_SUCCESS) { NRLog("[NR] Init_ProjectID ver=0x%02X ok", ver); inited = 1; }
    }
    if (!inited) { NRLog("[NR] Init_ProjectID failed"); return false; }

    if (s_directInit && s_shimInit)
    {
        NVSDK_NGX_PathListInfo pli = {}; const wchar_t* pl[1] = { data_path }; pli.Path = pl; pli.Length = 1;
        NVSDK_NGX_FeatureCommonInfo fci = {};
        fci.PathListInfo = pli;
        fci.LoggingInfo.LoggingLevel = NVSDK_NGX_LOGGING_LEVEL_OFF;
        NVSDK_NGX_Result r = s_shimInit((void*)s_directInit, APP_ID, data_path, m_dev12.p, 0x15, &fci);
        NRLog("[NR] snippet Init_Ext -> 0x%08X", (unsigned)r);
    }

    NVSDK_NGX_Result ra = s_alloc(&m_params);
    if (ra != NGX_SUCCESS || !m_params) { NRLog("[NR] AllocateParameters failed"); return false; }

    int style_int = 1;
    if (m_style == "default") style_int = 0;
    else if (m_style == "natural") style_int = 1;
    else if (m_style == "cinematic") style_int = 2;
    else style_int = atoi(m_style.c_str());

    m_params->Set("DLSSNR.Width", w);
    m_params->Set("DLSSNR.Height", h);
    m_params->Set("DLSSNR.Enabled", 1);
    m_params->Set("DLSSNR.Reset", 1);
    m_params->Set("DLSSNR.Style", style_int);
    m_params->Set("DLSSNR.Hint.Render.Preset", m_preset);
    m_params->Set("DLSSNR.Intensity", (float)m_intensity);
    m_params->Set("DLSSNR.LocalToneStrength", (float)m_tone);
    m_params->Set("DLSSNR.LocalStructureStrength", (float)m_structure);
    m_params->Set("DLSSNR.SkinStructureStrength", (float)m_skin);
    m_params->Set("DLSSNR.UseAutoMask", m_mask);
    m_params->Set("DLSSNR.UICorrection", 0);
    m_params->Set("DLSSNR.DepthInverted", 1);
    m_params->Set("DLSSNR.ScalingRatio", 1.0f);
    m_params->Set("DLSSNR.MVecScaleX", 1.0f);
    m_params->Set("DLSSNR.MVecScaleY", 1.0f);
    m_params->Set("DLSSNR.Color", m_nrIn.p);
    m_params->Set("DLSSNR.Output", m_nrOut.p);
    m_params->Set("DLSSNR.Backbuffer", m_nrOut.p);
    m_params->Set("DLSSNR.ColorSubrectBaseX", 0);
    m_params->Set("DLSSNR.ColorSubrectBaseY", 0);
    m_params->Set("DLSSNR.ColorSubrectWidth", w);
    m_params->Set("DLSSNR.ColorSubrectHeight", h);
    m_params->Set("DLSSNR.OutputSubrectBaseX", 0);
    m_params->Set("DLSSNR.OutputSubrectBaseY", 0);
    m_params->Set("DLSSNR.OutputSubrectWidth", w);
    m_params->Set("DLSSNR.OutputSubrectHeight", h);

    // CreateFeature (via shim if available) - the list must be OPEN here
    m_alloc12->Reset();
    m_list12->Reset(m_alloc12.p, nullptr);
    NVSDK_NGX_Result rc;
    if (s_nrCreate && s_shimCreate)
        rc = s_shimCreate((void*)s_nrCreate, m_list12.p, NR_FEATURE_ID, m_params, &m_feature);
    else
        rc = s_nrCreate(m_list12.p, NR_FEATURE_ID, m_params, &m_feature);
    if (rc != NGX_SUCCESS || !m_feature)
        { NRLog("[NR] CreateFeature(18) -> 0x%08X", (unsigned)rc); m_list12->Close(); return false; }

    // commit CreateFeature
    m_list12->Close();
    ID3D12CommandList* cmds[] = { m_list12.p };
    m_queue12->ExecuteCommandLists(1, cmds);
    UINT64 v = ++m_fenceValue;
    m_queue12->Signal(m_fence12.p, v);
    m_fence12->SetEventOnCompletion(v, m_fenceEvent);
    WaitForSingleObject(m_fenceEvent, 20000);

    NRLog("[NR] feature created %ux%u", w, h);
    return true;
}

bool CNRProcessor::UpdateParams()
{
    if (!m_params) return false;
    int style_int = 1;
    if (m_style == "default") style_int = 0;
    else if (m_style == "natural") style_int = 1;
    else if (m_style == "cinematic") style_int = 2;
    else style_int = atoi(m_style.c_str());
    m_params->Set("DLSSNR.Style", style_int);
    m_params->Set("DLSSNR.Hint.Render.Preset", m_preset);
    m_params->Set("DLSSNR.Intensity", (float)m_intensity);
    m_params->Set("DLSSNR.LocalToneStrength", (float)m_tone);
    m_params->Set("DLSSNR.LocalStructureStrength", (float)m_structure);
    m_params->Set("DLSSNR.SkinStructureStrength", (float)m_skin);
    m_params->Set("DLSSNR.UseAutoMask", m_mask);
    return true;
}

bool CNRProcessor::Process(ID3D11DeviceContext* pCtx11, ID3D11Texture2D* pFrame)
{
    if (!m_bInited || !m_bEnabled) return false;

    D3D11_TEXTURE2D_DESC fd;
    pFrame->GetDesc(&fd);
    if (!EnsureResources(fd.Width, fd.Height)) return false;

    CComPtr<ID3D11Device> dev11;
    pCtx11->GetDevice(&dev11);
    if (!EnsureInterop(dev11.p, pCtx11)) return false;
    if (!SetupNGX(fd.Width, fd.Height)) return false;
    if (m_bDirty) { UpdateParams(); m_bDirty = false; }

    // 1. copy the frame into the shared input texture (D3D11)
    pCtx11->CopyResource(m_in11.p, pFrame);

    // 2. order: D3D11 -> D3D12
    UINT64 v = m_fenceValue + 1;
    m_ctx4->Signal(m_fence11.p, v);
    m_queue12->Wait(m_fence12.p, v);

    // 3. NR on the D3D12 side
    m_alloc12->Reset();
    m_list12->Reset(m_alloc12.p, nullptr);

    D3D12_RESOURCE_BARRIER bars[6];
    UINT nb = 0;
    bars[nb++] = Trans(m_in12.p, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    bars[nb++] = Trans(m_nrIn.p, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_list12->ResourceBarrier(nb, bars);

    ID3D12DescriptorHeap* heaps[] = { m_heap.p };
    m_list12->SetDescriptorHeaps(1, heaps);
    m_list12->SetPipelineState(m_pso1.p);
    m_list12->SetComputeRootSignature(m_rs.p);
    UINT inc = m_dev12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_GPU_DESCRIPTOR_HANDLE h0 = m_heap->GetGPUDescriptorHandleForHeapStart();
    m_list12->SetComputeRootDescriptorTable(0, h0);                        // in12 SRV
    m_list12->SetComputeRootDescriptorTable(1, { h0.ptr + inc });          // nrIn UAV
    m_list12->Dispatch((m_w + 15) / 16, (m_h + 15) / 16, 1);

    // nrIn -> NPSR (NR reads)
    nb = 0;
    bars[nb++] = Trans(m_nrIn.p, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    m_list12->ResourceBarrier(nb, bars);

    m_params->Set("DLSSNR.Reset", m_bFirstFrame ? 1 : 0);
    m_bFirstFrame = false;
    if (s_nrEval && s_shimEval)
        s_shimEval((void*)s_nrEval, m_list12.p, m_feature, m_params, nullptr);
    else
        s_nrEval(m_list12.p, m_feature, m_params, nullptr);

    // nrOut -> NPSR (cs2 reads); out12 -> UAV (cs2 writes)
    nb = 0;
    bars[nb++] = Trans(m_nrOut.p, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    bars[nb++] = Trans(m_out12.p, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_list12->ResourceBarrier(nb, bars);

    m_list12->SetPipelineState(m_pso2.p);
    m_list12->SetComputeRootDescriptorTable(0, { h0.ptr + 2 * inc });      // nrOut SRV
    m_list12->SetComputeRootDescriptorTable(1, { h0.ptr + 3 * inc });      // out12 UAV
    m_list12->Dispatch((m_w + 15) / 16, (m_h + 15) / 16, 1);

    // restore shared textures to COMMON for D3D11
    nb = 0;
    bars[nb++] = Trans(m_in12.p, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    bars[nb++] = Trans(m_nrOut.p, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    bars[nb++] = Trans(m_out12.p, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    m_list12->ResourceBarrier(nb, bars);

    m_list12->Close();
    ID3D12CommandList* cmds[] = { m_list12.p };
    m_queue12->ExecuteCommandLists(1, cmds);

    // 4. order: D3D12 -> D3D11
    m_queue12->Signal(m_fence12.p, v + 1);
    m_ctx4->Wait(m_fence11.p, v + 1);
    m_fenceValue = v + 1;

    // 5. copy the NR output back into the frame (R8G8B8A8 -> backbuffer, hardware handles BGRA)
    if (m_psCopy && m_vsCopy && m_srvOut && m_sampler)
    {
        ID3D11RenderTargetView* rtv = nullptr;
        if (SUCCEEDED(dev11->CreateRenderTargetView(pFrame, nullptr, &rtv)))
        {
            D3D11_VIEWPORT vp = { 0, 0, (FLOAT)m_w, (FLOAT)m_h, 0.0f, 1.0f };
            pCtx11->OMSetRenderTargets(1, &rtv, nullptr);
            pCtx11->RSSetViewports(1, &vp);
            pCtx11->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            pCtx11->IASetInputLayout(nullptr);
            pCtx11->VSSetShader(m_vsCopy.p, nullptr, 0);
            pCtx11->PSSetShader(m_psCopy.p, nullptr, 0);
            pCtx11->PSSetShaderResources(0, 1, &m_srvOut);
            pCtx11->PSSetSamplers(0, 1, &m_sampler);
            pCtx11->Draw(3, 0);
            rtv->Release();
        }
    }
    else
    {
        // fallback: if the shader path failed, just leave the frame unchanged
        return false;
    }

    return true;
}
