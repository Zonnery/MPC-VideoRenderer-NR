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

// ---------------------------------------------------------------------------
// The NGX interface (NR feature 18) now lives in a separate worker process
// (nr_worker.exe). The renderer only shares RGBA16F textures with it and syncs
// via events. See SpawnWorker()/Process() below.
// ---------------------------------------------------------------------------

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
    // Also append to a log file next to the host exe, for easy verification.
    wchar_t exe[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    wchar_t* s = wcsrchr(exe, L'\\');
    if (s) *(s + 1) = 0;
    wchar_t lp[MAX_PATH];
    swprintf_s(lp, L"%lsnr_log.txt", exe);
    FILE* f = _wfopen(lp, L"a");
    if (f) { fputs(buf, f); fputs("\n", f); fclose(f); }
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
    // stop the NR worker process first
    if (m_bWorkerRunning)
    {
        if (m_hShutdown) SetEvent(m_hShutdown);
        if (m_piWorker.hProcess)
        {
            WaitForSingleObject(m_piWorker.hProcess, 3000);
            CloseHandle(m_piWorker.hProcess);
            CloseHandle(m_piWorker.hThread);
        }
        m_piWorker = {};
        m_bWorkerRunning = false;
    }
    if (m_hFrameReady) { CloseHandle(m_hFrameReady); m_hFrameReady = nullptr; }
    if (m_hFrameDone)  { CloseHandle(m_hFrameDone);  m_hFrameDone = nullptr; }
    if (m_hShutdown)   { CloseHandle(m_hShutdown);   m_hShutdown = nullptr; }
    if (m_fenceEvent) { CloseHandle(m_fenceEvent); m_fenceEvent = nullptr; }
    m_list12.Release();
    m_alloc12.Release();
    m_queue12.Release();
    m_dev12.Release();
    m_in11.Release(); m_out11.Release();
    m_in12.Release(); m_out12.Release();
    m_nrIn11.Release(); m_nrOut11.Release();
    m_nrIn.Release(); m_nrOut.Release();
    m_rs.Release(); m_pso1.Release(); m_pso2.Release(); m_heap.Release();
    m_fence11.Release(); m_fence12.Release(); m_ctx4.Release();
    m_vsCopy.Release(); m_psCopy.Release(); m_ilCopy.Release(); m_sampler.Release(); m_srvOut.Release();
    m_w = m_h = 0;
    m_bInited = false;
}

void CNRProcessor::Configure(bool enable, int style, int preset, int intensity,
                             int tone, int structure, int skin, int mask)
{
    m_bEnabled = enable;
    m_bDirty = true;
    switch (style) {
        case 0: m_style = "default"; break;
        case 1: m_style = "natural"; break;
        case 2: m_style = "cinematic"; break;
        default: m_style = "cinematic";
    }
    m_preset = preset;
    m_intensity = intensity;
    m_tone = tone;
    m_structure = structure;
    m_skin = skin;
    m_mask = mask;
}

bool CNRProcessor::CreateD3D12(ID3D11Device* pDevice11)
{
    // Enumerate adapters and pick the RTX 5090 (like the standalone player does),
    // matching nr_player's EnumAdapters1 + NVIDIA-vendor selection. This avoids any
    // difference between the D3D11 device's GetAdapter() object and the factory's.
    CComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) { NRLog("[NR] CreateDXGIFactory1 failed"); return false; }
    CComPtr<IDXGIAdapter1> chosen;
    for (UINT i = 0; factory->EnumAdapters1(i, &chosen) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d{};
        if (SUCCEEDED(chosen->GetDesc1(&d))) {
            NRLog("[NR] GPU[%d]: %ls (vendor=0x%04X)", i, d.Description, d.VendorId);
            if (d.VendorId == 0x10DE && wcsstr(d.Description, L"5090")) {
                NRLog("[NR] using adapter %d", i);
                break;
            }
        }
        chosen.Release();
    }
    if (!chosen) { NRLog("[NR] RTX 5090 not found"); return false; }

    if (FAILED(D3D12CreateDevice(chosen.p, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_dev12))))
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
    HRESULT hrCS = m_dev12->CreateSharedHandle(m_fence12.p, nullptr, GENERIC_ALL, nullptr, &fh);
    HRESULT hrOF = E_FAIL;
    if (SUCCEEDED(hrCS))
    {
        CComPtr<ID3D11Device5> dev5;
        if (SUCCEEDED(pDevice11->QueryInterface(IID_PPV_ARGS(&dev5))))
            hrOF = dev5->OpenSharedFence(fh, IID_PPV_ARGS(&m_fence11));
    }
    if (fh) CloseHandle(fh);
    if (!m_fence11) { NRLog("[NR] shared fence failed (CreateSharedHandle=0x%08X OpenSharedFence=0x%08X)", (unsigned)hrCS, (unsigned)hrOF); return false; }
    NRLog("[NR] fence shared ok (CreateSharedHandle=0x%08X OpenSharedFence=0x%08X)", (unsigned)hrCS, (unsigned)hrOF);

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

bool CNRProcessor::EnsureResources(UINT w, UINT h)
{
    if (w == m_w && h == m_h && m_nrIn) return true;

    // size changed: stop the worker so it can be re-spawned with the new size
    if (m_bWorkerRunning && (w != m_w || h != m_h))
    {
        if (m_hShutdown) SetEvent(m_hShutdown);
        if (m_piWorker.hProcess)
        {
            WaitForSingleObject(m_piWorker.hProcess, 3000);
            CloseHandle(m_piWorker.hProcess);
            CloseHandle(m_piWorker.hThread);
        }
        m_piWorker = {};
        m_bWorkerRunning = false;
    }
    m_w = w; m_h = h;

    // the NR shared textures + interop are (re)created lazily in EnsureInterop()
    m_nrIn11.Release(); m_nrOut11.Release(); m_nrIn.Release(); m_nrOut.Release();
    m_in11.Release(); m_out11.Release(); m_in12.Release(); m_out12.Release(); m_srvOut.Release();

    m_bFirstFrame = true;
    return true;
}

bool CNRProcessor::EnsureInterop(ID3D11Device* dev11, ID3D11DeviceContext* pCtx11)
{
    if (m_in11 && m_out11 && m_nrIn11 && m_nrOut11 && m_bWorkerRunning) return true;

    m_in11.Release(); m_out11.Release(); m_in12.Release(); m_out12.Release(); m_srvOut.Release();
    m_nrIn11.Release(); m_nrOut11.Release(); m_nrIn.Release(); m_nrOut.Release();

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = m_w; td.Height = m_h; td.MipLevels = 1; td.ArraySize = 1;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;

    // input: matches the backbuffer format (CopyResource in; SRV for the D3D12 read)
    td.Format = m_frameFormat;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev11->CreateTexture2D(&td, nullptr, &m_in11))) return false;

    // output: R8G8B8A8 (UAV for the D3D12 write + SRV for the D3D11 copy-back)
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    if (FAILED(dev11->CreateTexture2D(&td, nullptr, &m_out11))) return false;
    dev11->CreateShaderResourceView(m_out11.p, nullptr, &m_srvOut);

    // NR working textures (RGBA16F) - shared with the worker process
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    if (FAILED(dev11->CreateTexture2D(&td, nullptr, &m_nrIn11))) return false;
    if (FAILED(dev11->CreateTexture2D(&td, nullptr, &m_nrOut11))) return false;

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

    // share NR in/out -> D3D12 + inheritable NT handles for the worker
    SECURITY_ATTRIBUTES sa = {}; sa.nLength = sizeof(sa); sa.bInheritHandle = TRUE;
    HANDLE hnrIn = nullptr, hnrOut = nullptr;
    CComPtr<IDXGIResource1> nr1, nr2;
    if (SUCCEEDED(m_nrIn11->QueryInterface(IID_PPV_ARGS(&nr1))))
        nr1->CreateSharedHandle(&sa, GENERIC_ALL, nullptr, &hnrIn);
    if (SUCCEEDED(m_nrOut11->QueryInterface(IID_PPV_ARGS(&nr2))))
        nr2->CreateSharedHandle(&sa, GENERIC_ALL, nullptr, &hnrOut);
    if (hnrIn)  { m_dev12->OpenSharedHandle(hnrIn,  IID_PPV_ARGS(&m_nrIn));  }
    if (hnrOut) { m_dev12->OpenSharedHandle(hnrOut, IID_PPV_ARGS(&m_nrOut)); }

    if (!m_in12 || !m_out12 || !m_nrIn || !m_nrOut)
        { NRLog("[NR] interop share failed: in12=%p out12=%p nrIn=%p nrOut=%p", m_in12.p, m_out12.p, m_nrIn.p, m_nrOut.p); return false; }

    // descriptor views (heap slots [0..3])
    UINT inc = m_dev12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE base = m_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

    // [0] SRV in12
    srv.Format = m_frameFormat;
    m_dev12->CreateShaderResourceView(m_in12.p, &srv, { base.ptr + 0 * inc });
    // [1] UAV nrIn
    uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    m_dev12->CreateUnorderedAccessView(m_nrIn.p, nullptr, &uav, { base.ptr + 1 * inc });
    // [2] SRV nrOut
    srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    m_dev12->CreateShaderResourceView(m_nrOut.p, &srv, { base.ptr + 2 * inc });
    // [3] UAV out12
    uav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    m_dev12->CreateUnorderedAccessView(m_out12.p, nullptr, &uav, { base.ptr + 3 * inc });
    NRLog("[NR] views created; device removed reason=0x%08X", (unsigned)m_dev12->GetDeviceRemovedReason());

    // context4 (Signal/Wait) + D3D11 copy-back shader
    if (!m_ctx4)
        pCtx11->QueryInterface(IID_PPV_ARGS(&m_ctx4));

    // event query for CPU-side D3D11->D3D12 sync (D3D11 fence Signal is unreliable here)
    if (!m_query)
    {
        D3D11_QUERY_DESC qd = { D3D11_QUERY_EVENT, 0 };
        dev11->CreateQuery(&qd, &m_query);
    }

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

    // spawn the NR worker process (the NGX model runs there)
    if (!SpawnWorker(m_w, m_h, hnrIn, hnrOut)) { NRLog("[NR] SpawnWorker failed"); return false; }
    if (hnrIn) CloseHandle(hnrIn);
    if (hnrOut) CloseHandle(hnrOut);

    return true;
}

bool CNRProcessor::SpawnWorker(UINT w, UINT h, HANDLE hIn, HANDLE hOut)
{
    if (m_bWorkerRunning) return true;

    // resolve the worker exe path (next to the host exe, i.e. the MPC-BE install dir)
    wchar_t exe_path[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    wchar_t* slash = wcsrchr(exe_path, L'\\');
    if (slash) *(slash + 1) = 0;
    wcscat_s(exe_path, L"nr_worker.exe");

    // sync events (inheritable; auto-reset for frame handshakes, manual for shutdown)
    SECURITY_ATTRIBUTES sa = {}; sa.nLength = sizeof(sa); sa.bInheritHandle = TRUE;
    m_hFrameReady = CreateEventW(&sa, FALSE, FALSE, nullptr);
    m_hFrameDone  = CreateEventW(&sa, FALSE, FALSE, nullptr);
    m_hShutdown   = CreateEventW(&sa, TRUE,  FALSE, nullptr);

    int style_int = 1;
    if (m_style == "default") style_int = 0;
    else if (m_style == "natural") style_int = 1;
    else if (m_style == "cinematic") style_int = 2;
    else style_int = atoi(m_style.c_str());

    wchar_t cmd[1024];
    swprintf_s(cmd,
        L"\"%ls\" %llu %llu %u %u %llu %llu %llu %d %d %d %d %d",
        exe_path,
        (unsigned long long)(uintptr_t)hIn, (unsigned long long)(uintptr_t)hOut,
        w, h,
        (unsigned long long)(uintptr_t)m_hFrameReady,
        (unsigned long long)(uintptr_t)m_hFrameDone,
        (unsigned long long)(uintptr_t)m_hShutdown,
        style_int, m_preset, m_intensity, m_tone, m_structure);

    STARTUPINFOW si = {}; si.cb = sizeof(si);
    if (!CreateProcessW(nullptr, cmd, nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &m_piWorker))
    {
        NRLog("[NR] CreateProcessW failed err=%lu", (unsigned long)GetLastError());
        return false;
    }
    m_bWorkerRunning = true;
    NRLog("[NR] NR worker spawned (pid=%lu)", (unsigned long)m_piWorker.dwProcessId);
    return true;
}

bool CNRProcessor::Process(ID3D11DeviceContext* pCtx11, ID3D11Texture2D* pFrame)
{
    if (!m_bInited || !m_bEnabled) return false;

    D3D11_TEXTURE2D_DESC fd;
    pFrame->GetDesc(&fd);
    if (fd.Width < 32 || fd.Height < 32) return false; // skip placeholder/tiny frames
    if (!EnsureResources(fd.Width, fd.Height)) return false;

    CComPtr<ID3D11Device> dev11;
    pCtx11->GetDevice(&dev11);

    // track the backbuffer format (B8G8R8A8 on 8-bit, R10G10B10A2 on 10-bit output)
    {
        D3D11_TEXTURE2D_DESC fd; pFrame->GetDesc(&fd);
        if (fd.Format != m_frameFormat)
        {
            m_frameFormat = fd.Format;
            m_in11.Release(); m_in12.Release();  // recreate with the new format in EnsureInterop
            NRLog("[NR] frame format changed to %d", (int)fd.Format);
        }
    }

    if (!EnsureInterop(dev11.p, pCtx11)) return false;
    if (!m_bWorkerRunning) return false;

    // 1. copy the frame into the shared input texture (D3D11)
    pCtx11->CopyResource(m_in11.p, pFrame);

    // 2. CPU-side D3D11 -> D3D12 sync: event query waits for the CopyResource to complete on the GPU
    UINT64 v = m_fenceValue + 1;
    if (m_query)
    {
        pCtx11->End(m_query.p);
        while (pCtx11->GetData(m_query.p, nullptr, 0, 0) == S_FALSE) { Sleep(0); }
    }

    ID3D12DescriptorHeap* heaps[] = { m_heap.p };
    UINT inc = m_dev12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_GPU_DESCRIPTOR_HANDLE h0 = m_heap->GetGPUDescriptorHandleForHeapStart();
    D3D12_RESOURCE_BARRIER bars[4];
    ID3D12CommandList* cmds[] = { m_list12.p };

    // 3a. color convert B8G8R8A8 -> RGBA16F (pso1) into m_nrIn, then hand off to the worker
    m_alloc12->Reset();
    m_list12->Reset(m_alloc12.p, nullptr);
    UINT nb = 0;
    bars[nb++] = Trans(m_in12.p, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    bars[nb++] = Trans(m_nrIn.p, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_list12->ResourceBarrier(nb, bars);
    m_list12->SetDescriptorHeaps(1, heaps);
    m_list12->SetPipelineState(m_pso1.p);
    m_list12->SetComputeRootSignature(m_rs.p);
    m_list12->SetComputeRootDescriptorTable(0, h0);                        // in12 SRV
    m_list12->SetComputeRootDescriptorTable(1, { h0.ptr + inc });          // nrIn UAV
    m_list12->Dispatch((m_w + 15) / 16, (m_h + 15) / 16, 1);
    nb = 0;
    bars[nb++] = Trans(m_in12.p, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    bars[nb++] = Trans(m_nrIn.p, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    m_list12->ResourceBarrier(nb, bars);
    m_list12->Close();
    m_queue12->ExecuteCommandLists(1, cmds);
    // wait for pso1 to finish before the worker reads m_nrIn
    m_queue12->Signal(m_fence12.p, v + 1);
    m_fence12->SetEventOnCompletion(v + 1, m_fenceEvent);
    WaitForSingleObject(m_fenceEvent, 20000);

    // 3b. run the NR model in the worker process (m_nrIn -> m_nrOut)
    SetEvent(m_hFrameReady);
    if (WaitForSingleObject(m_hFrameDone, 20000) != WAIT_OBJECT_0)
    {
        NRLog("[NR] worker timeout");
        return false;
    }

    // 3c. color convert RGBA16F -> R8G8B8A8 (pso2) from m_nrOut
    m_alloc12->Reset();
    m_list12->Reset(m_alloc12.p, nullptr);
    nb = 0;
    bars[nb++] = Trans(m_nrOut.p, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    bars[nb++] = Trans(m_out12.p, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_list12->ResourceBarrier(nb, bars);
    m_list12->SetDescriptorHeaps(1, heaps);
    m_list12->SetPipelineState(m_pso2.p);
    m_list12->SetComputeRootSignature(m_rs.p);
    m_list12->SetComputeRootDescriptorTable(0, { h0.ptr + 2 * inc });      // nrOut SRV
    m_list12->SetComputeRootDescriptorTable(1, { h0.ptr + 3 * inc });      // out12 UAV
    m_list12->Dispatch((m_w + 15) / 16, (m_h + 15) / 16, 1);
    nb = 0;
    bars[nb++] = Trans(m_nrOut.p, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    bars[nb++] = Trans(m_out12.p, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    m_list12->ResourceBarrier(nb, bars);
    m_list12->Close();
    m_queue12->ExecuteCommandLists(1, cmds);

    // 4. CPU-side D3D12 -> D3D11 sync: wait for pso2 to complete before the D3D11 copy-back
    m_queue12->Signal(m_fence12.p, v + 2);
    m_fence12->SetEventOnCompletion(v + 2, m_fenceEvent);
    WaitForSingleObject(m_fenceEvent, 20000);
    m_fenceValue = v + 2;

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
        return false;
    }

    return true;
}
